/* Copyright (c) 2003 krzYszcz and others.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#include <stdio.h>
#include <string.h>
#ifdef UNIX
#include <unistd.h>
#endif
#ifdef NT
#include <io.h>
#endif
#include "m_pd.h"
#include "g_canvas.h"
#include "common/loud.h"
#include "common/grow.h"
#include "common/props.h"
#include "scriptlet.h"

//#define SCRIPTLET_DEBUG

#define SCRIPTLET_INISIZE   1024
#define SCRIPTLET_MARGIN      64
#define SCRIPTLET_MAXARGS      9  /* do not increase (parser's constraint) */
#define SCRIPTLET_MAXPUSH  20000  /* Tcl limit? LATER investigate */

enum { SCRIPTLET_CVOK, SCRIPTLET_CVUNKNOWN, SCRIPTLET_CVMISSING };

struct _scriptlet
{
    t_pd             *s_owner;
    t_glist          *s_glist;     /* containing glist (empty allowed) */
    t_symbol         *s_rptarget;  /* reply target */
    t_symbol         *s_cbtarget;  /* callback target */
    t_symbol         *s_item;
    t_scriptlet_cvfn  s_cvfn;      /* if empty, passing resolveall is a bug */
    t_canvas         *s_cv;        /* as returned by cvfn */
    int               s_cvstate;
    int     s_locked;  /* append lock, for filtering, when reading from file */
    int     s_size;
    char   *s_buffer;
    char    s_bufini[SCRIPTLET_INISIZE];
    char   *s_head;       /* ptr to the command part of a scriptlet */
    char   *s_tail;
    char    s_separator;  /* current separator, set before a new token */
    int     s_ac;                     /* the actual count */
    t_atom  s_av[SCRIPTLET_MAXARGS];  /* always padded with zeros (if used) */
};

static t_canvas *scriptlet_canvasvalidate(t_scriptlet *sp, int visedonly)
{
    t_canvas *cv;
    if (sp->s_cvstate == SCRIPTLET_CVUNKNOWN)
    {
	if (sp->s_cvfn)
	    cv = sp->s_cv = sp->s_cvfn(sp->s_owner);
	else
	{
	    bug("scriptlet_canvasvalidate");
	    return (0);
	}
	if (cv && (!visedonly || glist_isvisible(cv)))
	    sp->s_cvstate = SCRIPTLET_CVOK;
	else
	    sp->s_cvstate = SCRIPTLET_CVMISSING;
    }
    else cv = sp->s_cv;
    return (sp->s_cvstate == SCRIPTLET_CVOK ? cv : 0);
}

static int scriptlet_ready(t_scriptlet *sp)
{
    int len = sp->s_tail - sp->s_head;
    if (len > 0 && *sp->s_head && sp->s_cvstate != SCRIPTLET_CVMISSING)
    {
	if (len < SCRIPTLET_MAXPUSH)
	    return (1);
	else
	    loud_error(sp->s_owner,
		       "scriptlet too long to be pushed (%d bytes)", len);
    }
    return (0);
}

static int scriptlet_doappend(t_scriptlet *sp, char *buf)
{
    if (buf && !sp->s_locked)
    {
	int nprefix = sp->s_head - sp->s_buffer;
	int nused = sp->s_tail - sp->s_buffer;
	int newsize = nused + strlen(buf) + SCRIPTLET_MARGIN;
	if (newsize > sp->s_size)
	{
	    int nrequested = newsize;
	    sp->s_buffer = grow_withdata(&nrequested, &nused,
					 &sp->s_size, sp->s_buffer,
					 SCRIPTLET_INISIZE, sp->s_bufini,
					 sizeof(*sp->s_buffer));
	    if (nrequested != newsize)
	    {
		scriptlet_reset(sp);
		return (0);
	    }
	    sp->s_head = sp->s_buffer + nprefix;
	    sp->s_tail = sp->s_buffer + nused;
	}
	if (sp->s_separator && sp->s_tail > sp->s_head)
	    *sp->s_tail++ = sp->s_separator;
	*sp->s_tail = 0;
	strcpy(sp->s_tail, buf);
	sp->s_tail += strlen(sp->s_tail);
    }
    sp->s_separator = 0;
    return (1);
}

static char *scriptlet_dedot(t_scriptlet *sp, char *ibuf, char *obuf,
			     int resolveall, int visedonly,
			     int ac, t_atom *av, t_props *argprops)
{
    int len = 0;
    switch (*ibuf)
    {
    case '#':
	/* ac is ignored -- assuming av is padded to SCRIPTLET_MAXARGS atoms */
	if (resolveall)
	{
	    int which = ibuf[1] - '1';
	    if (which >= 0 && which < SCRIPTLET_MAXARGS)
	    {
		if (av)
		{
		    if (av[which].a_type == A_FLOAT)
		    {
			sprintf(obuf, "%g", av[which].a_w.w_float);
			len = 2;
		    }
		    else if (av[which].a_type == A_SYMBOL)
		    {
			strcpy(obuf, av[which].a_w.w_symbol->s_name);
			len = 2;
		    }
		}
	    }
	    else if (argprops)
	    {
		char *ptr;
		int cnt;
		for (ptr = ibuf + 1, cnt = 1; *ptr; ptr++, cnt++)
		{
		    char c = *ptr;
		    if ((c < 'A' || c > 'Z') && (c < 'a' || c > 'z'))
		    {
			cnt = 0;
			break;
		    }
		}
		if (cnt && (ptr = props_getvalue(argprops, ibuf + 1)))
		{
		    strcpy(obuf, ptr);
		    len = cnt;
		}
	    }
	}
	break;
    case '-':
	if (resolveall && sp->s_item)
	{
	    t_canvas *cv;
	    if (cv = scriptlet_canvasvalidate(sp, visedonly))
	    {
		sprintf(obuf, ".x%x.c.%s%x", (int)cv, sp->s_item->s_name,
			(int)sp->s_owner);
		len = 1;
	    }
	}
	break;
    case '^':
	if (resolveall)
	{
	    t_canvas *cv;
	    if (cv = scriptlet_canvasvalidate(sp, visedonly))
	    {
		sprintf(obuf, ".x%x", (int)cv);
		len = 1;
	    }
	}
	break;
    case '|':
	if (resolveall)
	{
	    strcpy(obuf, sp->s_cbtarget->s_name);
	    len = 1;
	}
	break;
    case '~':  /* FIXME, the dot-tilde stuff is purely experimental. */
	if (resolveall)
	{
	    t_canvas *cv;
	    if (cv = scriptlet_canvasvalidate(sp, 0))
	    {
		if (!strncmp(&ibuf[1], "tag", 3))
		{
		    t_rtext *rt;
		    if (cv->gl_owner && glist_isvisible(cv->gl_owner) &&
			cv->gl_owner->gl_editor &&
			(rt = glist_findrtext(cv->gl_owner, (t_object *)cv)))
			sprintf(obuf, "%s", rtext_gettag(rt));
		    else
			obuf[0] = 0;
		    len = 4;
		}
		else if (!strncmp(&ibuf[1], "owner", 5))
		{
		    if (cv->gl_owner && glist_isvisible(cv->gl_owner))
			sprintf(obuf, ".x%x", (int)cv->gl_owner);
		    else
			obuf[0] = 0;
		    len = 6;
		}
		else if (!strncmp(&ibuf[1], "root", 4))
		{
		    sprintf(obuf, ".x%x", (int)canvas_getrootfor(cv));
		    len = 5;
		}
		/* LATER find out when gl_<coords> are updated,
		   think how to better sync them to Tk. */
		else if (!strncmp(&ibuf[1], "x1", 2))
		{
		    sprintf(obuf, "%d", cv->gl_screenx1);
		    len = 3;
		}
		else if (!strncmp(&ibuf[1], "x2", 2))
		{
		    sprintf(obuf, "%d", cv->gl_screenx2);
		    len = 3;
		}
		else if (!strncmp(&ibuf[1], "y1", 2))
		{
		    sprintf(obuf, "%d", cv->gl_screeny1);
		    len = 3;
		}
		else if (!strncmp(&ibuf[1], "y2", 2))
		{
		    sprintf(obuf, "%d", cv->gl_screeny2);
		    len = 3;
		}
		else if (!strncmp(&ibuf[1], "edit", 4))
		{
		    sprintf(obuf, "%d", cv->gl_edit);
		    len = 5;
		}
		else if (!strncmp(&ibuf[1], "gop", 3))
		{
		    sprintf(obuf, "%d", glist_isgraph(cv));
		    len = 4;
		}
		else loud_error(sp->s_owner, "bad field '%s'", &ibuf[1]);
	    }
	}
	break;
    case '`':
	sprintf(obuf, "\\");
	len = 1;
	break;
    case ':':
	sprintf(obuf, ";");
	len = 1;
	break;
    case '(':
	sprintf(obuf, "{");
	len = 1;
	break;
    case ')':
	sprintf(obuf, "}");
	len = 1;
	break;
    case '<':
	if (resolveall)
	{
	    if (ibuf[1] == ':')
	    {
		sprintf(obuf, "{pd [concat ");
		len = 2;
	    }
	    else if (ibuf[1] == '|')
	    {
		sprintf(obuf, "{pd [concat %s ", sp->s_rptarget->s_name);
		len = 2;
	    }
	    else
	    {
		sprintf(obuf, "{pd [concat %s _cb ", sp->s_cbtarget->s_name);
		len = 1;
	    }
	}
	break;
    case '>':
	if (resolveall)
	{
	    sprintf(obuf, "\\;]}");
	    len = 1;
	}
	break;
    }
    return (len ? ibuf + len : 0);
}

int scriptlet_isempty(t_scriptlet *sp)
{
    return (!(sp->s_tail > sp->s_head && *sp->s_head));
}

void scriptlet_reset(t_scriptlet *sp)
{
    sp->s_cvstate = SCRIPTLET_CVUNKNOWN;
    sp->s_locked = 0;
    sp->s_separator = 0;
    strcpy(sp->s_buffer, "namespace eval ::toxy {\
 proc query {} {set ::toxy::reply [\n");
    sp->s_head = sp->s_tail = sp->s_buffer + strlen(sp->s_buffer);
}

void scriptlet_prealloc(t_scriptlet *sp, int sz, int mayshrink)
{
    if (sz < SCRIPTLET_INISIZE)
	sz = SCRIPTLET_INISIZE;
    if (sz < sp->s_size && mayshrink)
    {
	if (sp->s_buffer != sp->s_bufini)
	    freebytes(sp->s_buffer, sp->s_size * sizeof(*sp->s_buffer));
	else
	    bug("scriptlet_prealloc");
	sp->s_size = SCRIPTLET_INISIZE;
	sp->s_buffer = sp->s_bufini;
    }
    if (sz > sp->s_size)
	sp->s_buffer = grow_nodata(&sz, &sp->s_size, sp->s_buffer,
				   SCRIPTLET_INISIZE, sp->s_bufini,
				   sizeof(*sp->s_buffer));
    scriptlet_reset(sp);
}

int scriptlet_addstring(t_scriptlet *sp, char *ibuf,
			int resolveall, int visedonly,
			int ac, t_atom *av, t_props *argprops)
{
    int result = 1;
    char *bp = ibuf, *ep = ibuf, *ep1;
    char dotbuf[64];  /* LATER reestimation */
    if (!sp->s_separator)
	sp->s_separator = ' ';
    while (*ep)
    {
	if (*ep == '.'
	    && (ep1 = scriptlet_dedot(sp, ep + 1, dotbuf,
				      resolveall, visedonly, ac, av, argprops)))
	{
	    *ep = 0;
	    if (!(result = scriptlet_doappend(sp, bp)))
		break;
	    *ep = '.';
	    if (!(result = scriptlet_doappend(sp, dotbuf)))
		break;
	    bp = ep = ep1;
	}
	else ep++;
    }
    if (result)
	result = scriptlet_doappend(sp, bp);
    sp->s_separator = 0;
    return (result);
}

int scriptlet_addfloat(t_scriptlet *sp, t_float f)
{
    char buf[64];
    if (!sp->s_separator)
	sp->s_separator = ' ';
    sprintf(buf, "%g ", f);
    return (scriptlet_doappend(sp, buf));
}

int scriptlet_add(t_scriptlet *sp,
		  int resolveall, int visedonly, int ac, t_atom *av)
{
    while (ac--)
    {
	int result = 1;
	if (av->a_type == A_SYMBOL)
	    result = scriptlet_addstring(sp, av->a_w.w_symbol->s_name,
					 resolveall, visedonly, 0, 0, 0);
	else if (av->a_type == A_FLOAT)
	    result = scriptlet_addfloat(sp, av->a_w.w_float);
	if (!result)
	    return (0);
	av++;
    }
    return (1);
}

void scriptlet_setseparator(t_scriptlet *sp, char c)
{
    sp->s_separator = c;
}

void scriptlet_push(t_scriptlet *sp)
{
    if (scriptlet_ready(sp))
    {
	char *tail = sp->s_tail;
	strcpy(tail, "\n");
	sys_gui(sp->s_head);
	*tail = 0;
    }
}

void scriptlet_qpush(t_scriptlet *sp)
{
    if (scriptlet_ready(sp))
    {
	char buf[MAXPDSTRING];
	char *tail = sp->s_tail;
	strcpy(tail, "]}}\n");
	sys_gui(sp->s_buffer);
	*tail = 0;
	sprintf(buf, "after 0 {::toxy::query}\nvwait ::toxy::reply\n\
 pd [concat %s _rp $::toxy::reply \\;]\n", sp->s_rptarget->s_name);
	sys_gui(buf);
    }
}

/* Non-substituting -- LATER think if this is likely to cause any confusion.
   Especially, consider the widget_vis() vs. widget_update() case. */
void scriptlet_vpush(t_scriptlet *sp, char *varname)
{
    if (scriptlet_ready(sp))
    {
	char *tail = sp->s_tail;
	strcpy(tail, "}\n");
	sys_vgui("set ::toxy::%s { ", varname);
	sys_gui(sp->s_head);
	*tail = 0;
    }
}

int scriptlet_evaluate(t_scriptlet *insp, t_scriptlet *outsp,
		       int visedonly, int ac, t_atom *av, t_props *argprops)
{
    if (scriptlet_ready(insp))
    {
	t_atom *ap;
	int i;
	char *bp;
	char separator = 0;
	insp->s_ac = ac;
	for (i = 0, ap = insp->s_av; i < SCRIPTLET_MAXARGS; i++, ap++)
	{
	    if (ac)
	    {
		if (av->a_type == A_FLOAT ||
		    (av->a_type == A_SYMBOL && av->a_w.w_symbol))
		    *ap = *av;
		else
		    SETFLOAT(ap, 0);
		ac--; av++;
	    }
	    else SETFLOAT(ap, 0);
	}
	/* FIXME pregrowing of the transient scriptlet */
	scriptlet_reset(outsp);
	/* LATER abstract this into scriptlet_parse() */
	bp = insp->s_head;
	while (*bp)
	{
	    if (*bp == '\n')
		separator = '\n';
	    else if (*bp == ' ' || *bp == '\t')
	    {
		if (!separator) separator = ' ';
	    }
	    else
	    {
		int done = 1;
		char *ep = bp;
		char c = ' ';
		while (*++ep)
		{
		    if (*ep == ' ' || *bp == '\t' || *ep == '\n')
		    {
			done = 0;
			c = *ep;
			*ep = 0;
			break;
		    }
		}
		outsp->s_separator = separator;
		scriptlet_addstring(outsp, bp, 1, visedonly,
				    ac, insp->s_av, argprops);
		if (done)
		    break;
		*ep = c;
		bp = ep;
		separator = (c == '\t' ? ' ' : c);
	    }
	    bp++;
	}
	return (outsp->s_cvstate != SCRIPTLET_CVMISSING);
    }
    else return (0);
}

/* utility function to be used in a comment-parsing callback */
char *scriptlet_nextword(char *buf)
{
    while (*++buf)
    {
	if (*buf == ' ' || *buf == '\t')
	{
	    char *ptr = buf + 1;
	    while (*ptr == ' ' || *ptr == '\t') ptr++;
	    *buf = 0;
	    return (*ptr ? ptr : 0);
	}
    }
    return (0);
}

static int scriptlet_doread(t_scriptlet *sp, t_pd *caller, FILE *fp,
			    char *rc, char *builtin, t_scriptlet_cmntfn cmntfn)
{
    t_scriptlet *outsp = sp, *newsp;
    char buf[MAXPDSTRING];
    if (!caller) caller = sp->s_owner;
    while ((fp && !feof(fp) && fgets(buf, MAXPDSTRING - 1, fp))
	   || builtin)
    {
	char *ptr;
	if (builtin)
	{
	    int i;
	    for (i = 0, ptr = buf; i < MAXPDSTRING - 1; i++, ptr++)
	    {
		if ((*ptr = (*builtin ? *builtin : '\n')) == '\n')
		{
		    ptr[1] = 0;
		    if (*builtin) builtin++;
		    if (!*builtin) builtin = 0;
		    break;
		}
		else builtin++;
	    }
	}
	ptr = buf;
	while (*ptr == ' ' || *ptr == '\t') ptr++;
	if (*ptr == '#')
	{
	    if (cmntfn)
	    {
		char sel = *++ptr;
		if (sel && sel != '\n')
		{
		    ptr++;
		    while (*ptr == ' ' || *ptr == '\t') ptr++;
		    if (*ptr == '\n')
			*ptr = 0;
		    if (*ptr)
		    {
			char *ep = ptr + strlen(ptr) - 1;
			while (*ep == ' ' || *ep == '\t' || *ep == '\n')
			    ep--;
			ep[1] = 0;
		    }
		    newsp = cmntfn(caller, rc, sel, ptr);
		    if (newsp == SCRIPTLET_UNLOCK)
			outsp->s_locked = 0;
		    else if (newsp == SCRIPTLET_LOCK)
			outsp->s_locked = 1;
		    else if (newsp != outsp)
		    {
			outsp->s_locked = 0;
			scriptlet_reset(outsp = newsp);
		    }
		}
	    }
	}
	else if (*ptr && *ptr != '\n')
	    scriptlet_doappend(outsp, buf);
    }
    outsp->s_locked = 0;
    return (SCRIPTLET_OK);
}

/* Load particular section(s) from buffer (skip up to an unlocking comment,
   keep appending up to a locking comment, repeat). */
int scriptlet_rcparse(t_scriptlet *sp, t_pd *caller, char *rc, char *contents,
		      t_scriptlet_cmntfn cmntfn)
{
    int result;
    sp->s_locked = 1;  /* see scriptlet_doread() above for unlocking scheme */
    result = scriptlet_doread(sp, caller, 0, rc, contents, cmntfn);
    return (result);
}

int scriptlet_rcload(t_scriptlet *sp, t_pd *caller, char *rc, char *ext,
		     char *builtin, t_scriptlet_cmntfn cmntfn)
{
    int result;
    char filename[MAXPDSTRING], buf[MAXPDSTRING], *nameptr, *dir;
    int fd;
    if (sp->s_glist)
	dir = canvas_getdir(sp->s_glist)->s_name;
    else
	dir = "";
    if ((fd = open_via_path(dir, rc, ext, buf, &nameptr, MAXPDSTRING, 0)) < 0)
    {
	result = SCRIPTLET_NOFILE;
    }
    else
    {
	FILE *fp;
    	close(fd);
	if (nameptr != buf)
	{
	    strcpy(filename, buf);
	    strcat(filename, "/");
	    strcat(filename, nameptr);
	    sys_bashfilename(filename, filename);
	}
	else sys_bashfilename(nameptr, filename);
	if (fp = fopen(filename, "r"))
	{
	    result = scriptlet_doread(sp, caller, fp, rc, 0, cmntfn);
	    fclose(fp);
	}
	else
	{
	    bug("scriptlet_rcload");
	    result = SCRIPTLET_NOFILE;
	}
    }
    if (result != SCRIPTLET_OK)
    {
	scriptlet_doread(sp, caller, 0, rc, builtin, cmntfn);
    }
    return (result);
}

int scriptlet_read(t_scriptlet *sp, t_symbol *fn)
{
    int result;
    FILE *fp;
    char buf[MAXPDSTRING];
    post("loading scriptlet file \"%s\"", fn->s_name);
    if (sp->s_glist)
	canvas_makefilename(sp->s_glist, fn->s_name, buf, MAXPDSTRING);
    else
	strncpy(buf, fn->s_name, MAXPDSTRING);
    sys_bashfilename(buf, buf);
    if (fp = fopen(buf, "r"))
    {
	scriptlet_reset(sp);
	result = scriptlet_doread(sp, 0, fp, 0, 0, 0);
	fclose(fp);
    }
    else
    {
	loud_error(sp->s_owner, "error while loading file \"%s\"", fn->s_name);
    	result = SCRIPTLET_NOFILE;
    }
    return (result);
}

int scriptlet_write(t_scriptlet *sp, t_symbol *fn)
{
    int size = sp->s_tail - sp->s_head;
    if (size > 0 && *sp->s_head)
    {
	FILE *fp;
	char buf[MAXPDSTRING];
	post("saving scriptlet file \"%s\"", fn->s_name);
	if (sp->s_glist)
	    canvas_makefilename(sp->s_glist, fn->s_name, buf, MAXPDSTRING);
	else
	    strncpy(buf, fn->s_name, MAXPDSTRING);
	sys_bashfilename(buf, buf);
	if (fp = fopen(buf, "w"))
	{
	    int result = fwrite(sp->s_head, 1, size, fp);
	    fclose(fp);
	    if (result == size)
		return (SCRIPTLET_OK);
	}
	loud_error(sp->s_owner, "error while saving file \"%s\"", fn->s_name);
	return (fp ? SCRIPTLET_BADFILE : SCRIPTLET_NOFILE);
    }
    else
    {
	loud_warning(sp->s_owner, "empty scriptlet not written");
	return (SCRIPTLET_IGNORED);
    }
}

char *scriptlet_getcontents(t_scriptlet *sp, int *lenp)
{
    *lenp = sp->s_tail - sp->s_head;
    return (sp->s_head);
}

char *scriptlet_getbuffer(t_scriptlet *sp, int *sizep)
{
    *sizep = sp->s_size;
    return (sp->s_buffer);
}

void scriptlet_setowner(t_scriptlet *sp, t_pd *owner)
{
    sp->s_owner = owner;
}

void scriptlet_clone(t_scriptlet *to, t_scriptlet *from)
{
    scriptlet_reset(to);
    to->s_separator = ' ';
    /* LATER add a flag to optionally use from's buffer with refcount */
    scriptlet_doappend(to, from->s_head);
}

void scriptlet_append(t_scriptlet *to, t_scriptlet *from)
{
    to->s_separator = ' ';
    scriptlet_doappend(to, from->s_head);
}

void scriptlet_free(t_scriptlet *sp)
{
    if (sp)
    {
	if (sp->s_buffer != sp->s_bufini)
	    freebytes(sp->s_buffer, sp->s_size * sizeof(*sp->s_buffer));
	freebytes(sp, sizeof(*sp));
    }
}

/* The parameter 'gl' (null accepted) is necessary, because the 's_glist'
   field, if implicitly set, would be dangerous (after a glist is gone)
   and confusing (current directory used for i/o of a global scriptlet). */
t_scriptlet *scriptlet_new(t_pd *owner, t_symbol *rptarget, t_symbol *cbtarget,
			   t_symbol *item, t_glist *gl, t_scriptlet_cvfn cvfn)
{
    t_scriptlet *sp = getbytes(sizeof(*sp));
    if (sp)
    {
	static int configured = 0;
	if (!configured)
	{
	    sys_gui("image create bitmap ::toxy::img::empty -data {}\n");
	}
	sp->s_owner = owner;
	sp->s_glist = gl;
	sp->s_rptarget = rptarget;
	sp->s_cbtarget = cbtarget;
	sp->s_item = item;
	sp->s_cvfn = cvfn;
	sp->s_size = SCRIPTLET_INISIZE;
	sp->s_buffer = sp->s_bufini;
	scriptlet_reset(sp);
    }
    return (sp);
}