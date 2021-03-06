/* Copyright (c) 2003 krzYszcz and others.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* LATER use hasfeeders */

#include "m_pd.h"
#include "sickle/sic.h"

#define GREATERTHANEQ_DEFRHS  0.  /* CHECKED */


typedef t_sic t_greaterthaneq;
static t_class *greaterthaneq_class;

static t_int *greaterthaneq_perform(t_int *w)
{
    int nblock = (int)(w[1]);
    t_float *in1 = (t_float *)(w[2]);
    t_float *in2 = (t_float *)(w[3]);
    t_float *out = (t_float *)(w[4]);
    while (nblock--)
    {
	t_float f1 = *in1++;
	t_float f2 = *in2++;
    *out++ = (f1 >= f2); /* the great code */
    }
    return (w + 5);
}

static void greaterthaneq_dsp(t_greaterthaneq *x, t_signal **sp)
{
    dsp_add(greaterthaneq_perform, 4, sp[0]->s_n,
	    sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec);
}

static void *greaterthaneq_new(t_symbol *s, int ac, t_atom *av)
{
    t_greaterthaneq *x = (t_greaterthaneq *)pd_new(greaterthaneq_class);
    sic_inlet((t_sic *)x, 1, GREATERTHANEQ_DEFRHS, 0, ac, av);
    outlet_new((t_object *)x, &s_signal);
    return (x);
}

void greaterthaneq_tilde_setup(void)
{
    greaterthaneq_class = class_new(gensym("greaterthaneq~"),
			      (t_newmethod)greaterthaneq_new, 0,
			      sizeof(t_greaterthaneq), 0, A_GIMME, 0);
    sic_setup(greaterthaneq_class, greaterthaneq_dsp, SIC_FLOATTOSIGNAL);
}
