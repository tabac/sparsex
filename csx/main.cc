/*
 * main.cc -- Main program for invoking CSX.
 *
 * Copyright (C) 2011-2012, Computing Systems Laboratory (CSLab), NTUA.
 * Copyright (C) 2011-2013, Vasileios Karakasis
 * Copyright (C) 2012-2013, Athena Elafrou
 * All rights reserved.
 *
 * This file is distributed under the BSD License. See LICENSE.txt for details.
 */
#include "../C-API/mattype.h"
#include "matrix_loading.h"
#include "sparse_internal.h"
#include "csx_get_set.h"
#include "csx_util.h"
#include "runtime.h"
#include "csx_build.h"

#include <cstdio>
#include <cfloat>

extern template bool GetValueCsx<index_t, value_t>(void *, index_t, index_t, value_t *);
extern template bool SetValueCsx<index_t, value_t>(void *, index_t, index_t, value_t);
extern template void PutSpmMt<value_t>(spm_mt_t *);

static const char *program_name;

//
// FIXME: This is a duplicate of calc_imbalance() in libspmv
// 
static double CalcImbalance(void *m)
{
    spm_mt_t *spm_mt = (spm_mt_t *) m;
    size_t i;
    
    double min_time = DBL_MAX;
    double max_time = 0.0;
    double total_time = 0.0;
    size_t worst = -1;
    for (i = 0; i < spm_mt->nr_threads; ++i) {
        spm_mt_thread_t *spm = &(spm_mt->spm_threads[i]);
        double thread_time = spm->secs;
        printf("thread %zd: %f\n", i, thread_time);
        total_time += thread_time;
        if (thread_time > max_time) {
            max_time = thread_time;
            worst = i;
        }

        if (thread_time < min_time)
            min_time = thread_time;
    }

    double ideal_time = total_time / spm_mt->nr_threads;
    printf("Worst thread: %zd\n", worst);
    printf("Expected perf. improvement: %.2f %%\n",
           100*(max_time / ideal_time - 1));
    return (max_time - min_time) / min_time;
}

void PrintUsage(std::ostream &os)
{
    os << "Usage: " << program_name
       << " [-s] [-b] <mmf_file> ...\n"
       // << "\t-s    Use CSX for symmetric matrices.\n"
       // << "\t-b    Disable the split-blocks optimization.\n"
       << "\t-h    Print this help message and exit.\n";
}

int main(int argc, char **argv)
{   
    char c;
    // bool split_blocks = true;
    // bool symmetric = false;
    spm_mt_t *spm_mt;

    program_name = argv[0];
    while ((c = getopt(argc, argv, "bsh")) != -1) {
        switch (c) {
        // case 'b':
        //     split_blocks = false;
        //     break;
        // case 's':
        //     symmetric = true;
        //     break;
        case 'h':
            PrintUsage(std::cerr);
            exit(0);
        default:
            PrintUsage(std::cerr);
            exit(1);
        }
    }
    
    int remargc = argc - optind; // remaining arguments
    if (remargc < 1) {
        PrintUsage(std::cerr);
        exit(1);
    }
    argv = &argv[optind];

    // Initialization of runtime configuration
    RuntimeConfiguration &rt_config = RuntimeConfiguration::GetInstance();
    RuntimeContext &rt_context = RuntimeContext::GetInstance();

    rt_config.LoadFromEnv();
    rt_context.SetRuntimeContext(rt_config);

<<<<<<< HEAD
    // Load matrix
    SparseInternal<uint64_t, double> *spms = NULL;
    //SparsePartitionSym<uint64_t, double> *spms_sym = NULL;

    if (rt_config.GetProperty<bool>(RuntimeConfiguration::MatrixSymmetric)) {
        spms = LoadMMF_mt<uint64_t, double>(argv[0], rt_context.GetNrThreads());
    }
    
    for (int i = 0; i < remargc; i++) {    
        std::cout << "=== BEGIN BENCHMARK ===" << std::endl; 
        if (spms)
            spm_mt = BuildCsx(spms);
        // else
        // spm_mt = BuildCsxSym(spms_sym, rt_context, csx_context);
        CheckLoop(spm_mt, argv[i]);
=======
    // Load matrix    
    SparseInternal<index_t, value_t> *spms = NULL;
    SparsePartitionSym<index_t, value_t> *spms_sym = NULL;

    csx::Timer timer1;
    timer1.Start();
    if (!csx_context.IsSymmetric()) {
        spms = LoadMMF_mt<index_t, value_t>(argv[0], rt_context.GetNrThreads());
    } else {
        spms_sym = LoadMMF_Sym_mt<index_t, value_t>(argv[0], rt_context.GetNrThreads());
    }

    for (int i = 0; i < remargc; i++) {    
        std::cout << "=== BEGIN BENCHMARK ===" << std::endl; 
        double pre_time;
        if (!symmetric)
            spm_mt = BuildCsx(spms, csx_context, pre_time);
        else 
            spm_mt = BuildCsxSym(spms_sym, csx_context, pre_time);
        timer1.Pause();
        pre_time = timer1.ElapsedTime();
        std::cout << pre_time << std::endl;
        //CheckLoop(spm_mt, argv[i]);
>>>>>>> b66e7fb93f8284b703d2cd46a40c6347a387bd06
        std::cerr.flush();
        //BenchLoop(spm_mt, argv[i]);
        //double imbalance = CalcImbalance(spm_mt);
        //std::cout << "Load imbalance: " << 100*imbalance << "%\n";
        std::cout << "=== END BENCHMARK ===" << std::endl;

        /* Get/Set testing */
        csx::Timer timer;
        timer.Start();
        MMF<index_t, value_t> mmf(argv[i]);
        MMF<index_t, value_t>::iterator iter = mmf.begin();
        MMF<index_t, value_t>::iterator iter_end = mmf.end();
        value_t value;
        for (;iter != iter_end; ++iter) {
            index_t row = (*iter).row;
            index_t col = (*iter).col;
            if (!symmetric)
                GetValueCsx<index_t, value_t>(spm_mt, row, col, &value);
            else
                GetValueCsxSym<index_t, value_t>(spm_mt, row, col, &value);
            assert((*iter).val == value);
        }
        timer.Pause();
        pre_time = timer.ElapsedTime();
        std::cout << pre_time << std::endl;
        /* Cleanup */
        PutSpmMt<value_t>(spm_mt);
    }
    
    return 0;
}

// vim:expandtab:tabstop=8:shiftwidth=4:softtabstop=4
