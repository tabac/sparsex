/*
 * Copyright (C) 2014, Computing Systems Laboratory (CSLab), NTUA.
 * Copyright (C) 2014, Athena Elafrou
 * All rights reserved.
 *
 * This file is distributed under the BSD License. See LICENSE.txt for details.
 */

/**
 * \file Barrier.cpp
 * \brief A centralized barrier with timeout
 *
 * \author Computing Systems Laboratory (CSLab), NTUA
 * \date 2011&ndash;2014
 * \copyright This file is distributed under the BSD License. See LICENSE.txt
 * for details.
 */

#include <sparsex/internals/Barrier.hpp>

using namespace std;

namespace sparsex {
  namespace runtime {

    atomic<int> global_sense;
    atomic<int> barrier_cnt;

    static inline int do_spin(int *local_sense)
    {
      volatile int i, spin_cnt = BARRIER_TIMEOUT;

      for (i = 0; i < spin_cnt; i++) {
        if ((*local_sense) == global_sense.load()) {
	  return 0;
        } else {
	  __asm volatile ("" : : : "memory");
        }
      }

      return 1;
    }

    void centralized_barrier(int *local_sense, size_t nr_threads)
    {
      // each processor toggles its own sense
      *local_sense = !(*local_sense);
      if (atomic_fetch_sub(&barrier_cnt, 1) == 1) {
        barrier_cnt.store(nr_threads);
        // last processor toggles global sense
        global_sense.store(*local_sense);
        // wake up the other threads
        futex_wake((int *) &global_sense, nr_threads);
      } else {
        while (*local_sense != global_sense.load()) {
	  if (do_spin(local_sense))
	    futex_wait((int *) &global_sense, !(*local_sense));
        }
      }
    }

  } // end of namespace runtime
} // end of namespace sparsex
