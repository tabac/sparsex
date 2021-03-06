/*
 * Copyright (C) 2013, Computing Systems Laboratory (CSLab), NTUA.
 * Copyright (C) 2013-2014, Athena Elafrou
 * Copyright (C) 2014,      Vasileios Karakasis
 * All rights reserved.
 *
 * This file is distributed under the BSD License. See LICENSE.txt for details.
 */

/**
 * \file matvec.c
 * \brief Sparse matrix routines.
 *
 * \author Computing Systems Laboratory (CSLab), NTUA
 * \date 2011&ndash;2014
 * \copyright This file is distributed under the BSD License. See LICENSE.txt
 * for details.
 */

#include <sparsex/matvec.h>
#include <sparsex/internals/CsxKernels.hpp>
#include <sparsex/internals/Facade.hpp>

#include <unistd.h>
#include <float.h>

/**
 *  \brief The sparse matrix internal representation.
 */
struct matrix {
  spx_index_t nrows, ncols, nnz;
  int symmetric;              /**< Flag that indicates whether the symmetric
				 version of CSX will be used */
  spx_perm_t *permutation;    /**< The permutation, in case the matrix has
				 been reordered */
  void *csx;                  /**< The tuned matrix representation, i.e. the
				 input matrix transformed to the CSX format */
};

/**
 *  \brief The input matrix internal representation.
 */
struct input {
  spx_index_t nrows, ncols, nnz;
  char type;      /**< A character that stores the format of the input matrix
		     ('C' for CSR or 'M' for MMF) */
  void *mat;      /**< The input matrix representation */
};

/**
 *  \brief A representation of a matrix partitioning.
 */
struct partition {
  size_t nr_partitions;
  size_t *parts;  /**< Partition size in bytes for interleaved allocation */
  int *nodes;     /**< Numa nodes for interleaved allocation */
  int *affinity;
  spx_index_t *row_start;
  spx_index_t *row_end;
};

/**
 *  \brief Allocates an input structure.
 */
static void input_alloc_struct(spx_input_t **A)
{
  *A = spx_malloc(spx_input_t, sizeof(spx_input_t));
  (*A)->nrows = 0;
  (*A)->ncols = 0;
  (*A)->nnz = 0;
  (*A)->type = 'U';
  (*A)->mat = SPX_INVALID_INPUT;
}

/**
 *  \brief Frees an input structure.
 */
static void input_free_struct(spx_input_t *A)
{
  if (A->mat) {
    if (A->type == 'C') {
      DestroyCSR(A->mat);
    } else {
      DestroyMMF(A->mat);
    }
  }

  spx_free(A);
}

/**
 *  \brief Allocates a matrix structure.
 */
static void mat_alloc_struct(spx_matrix_t **A)
{
  *A = spx_malloc(spx_matrix_t, sizeof(spx_matrix_t));
  (*A)->nrows = 0;
  (*A)->ncols = 0;
  (*A)->nnz = 0;
  (*A)->permutation = SPX_INVALID_PERM;
  (*A)->csx = SPX_INVALID_MAT;
  (*A)->symmetric = 0;
}

/**
 *  \brief Frees a matrix structure.
 */
static void mat_free_struct(spx_matrix_t *A)
{
  if (A->csx) {
    DestroyCsx(A->csx);
  }

  if (A->permutation) {
    spx_free(A->permutation);
  }

  spx_free(A);
}

/**
 *  \brief Allocates a partition structure.
 */
static void part_alloc_struct(spx_partition_t **p)
{
  *p = spx_malloc(spx_partition_t, sizeof(spx_partition_t));
  (*p)->nr_partitions = 0;
  (*p)->parts = NULL;
  (*p)->nodes = NULL;
  (*p)->affinity = NULL;
  (*p)->row_start = NULL;
  (*p)->row_end = NULL;
}

/**
 *  \brief Frees a partition structure.
 */
static void part_free_struct(spx_partition_t *p)
{
  if (p->parts) {
    spx_free(p->parts);
  }

  if (p->nodes) {
    spx_free(p->nodes);
  }

  if (p->affinity) {
    spx_free(p->affinity);
  }

  if (p->row_start) {
    spx_free(p->row_start);
  }

  if (p->row_end) {
    spx_free(p->row_end);
  }

  spx_free(p);
}

spx_input_t *spx_input_load_csr(const spx_index_t *rowptr,
                                const spx_index_t *colind, 
                                const spx_value_t *values,
                                spx_index_t nrows, spx_index_t ncols, ...)
{
  /* Check optional arguments */
  va_list ap;
  va_start(ap, ncols);
  spx_option_t indexing = va_arg(ap, spx_option_t);
  va_end(ap);

  indexing = indexing - SPX_INDEX_ZERO_BASED;
  if (!check_indexing(indexing)) {
    indexing = 0;
  }

  /* Check validity of input arguments */
  if (!check_mat_dim(nrows) || !check_mat_dim(ncols)) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix dimensions");
    return SPX_INVALID_INPUT;
  }

  if (!rowptr) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid rowptr argument");
    return SPX_INVALID_INPUT;
  }

  if (!colind) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid colind argument");
    return SPX_INVALID_INPUT;
  }

  if (!values) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid values argument");
    return SPX_INVALID_INPUT;
  }

  /* Create CSR wrapper */
  spx_input_t *A = SPX_INVALID_INPUT;
  input_alloc_struct(&A);
  A->type = 'C';
  A->nrows = nrows;
  A->ncols = ncols;
  A->nnz = rowptr[nrows] - indexing;
  A->mat = CreateCSR(rowptr, colind, values, nrows, ncols, !indexing);
  if (!A->mat) {
    input_free_struct(A);
    SETERROR_1(SPX_ERR_INPUT_MAT, "creating CSR wrapper failed");
    return SPX_INVALID_INPUT;
  }

  return A;
}

spx_input_t *spx_input_load_mmf(const char *filename)
{
  /* Check validity of input argument */
  if (!filename) {
    SETERROR_0(SPX_ERR_FILE);
    return SPX_INVALID_INPUT;
  }

  /* This is not a 100% safe check */
  if (access(filename, F_OK | R_OK) == -1) {
    SETERROR_0(SPX_ERR_FILE);
    return SPX_INVALID_INPUT;
  }

  /* Load matrix */
  spx_input_t *A = SPX_INVALID_INPUT;
  input_alloc_struct(&A);
  A->type = 'M';
  A->mat = CreateMMF(filename, &(A->nrows), &(A->ncols), &(A->nnz));
  if (!A->mat) {
    input_free_struct(A);
    SETERROR_1(SPX_ERR_INPUT_MAT, "loading matrix from MMF file failed");
    return SPX_INVALID_INPUT;
  }

  return A;
}

spx_error_t spx_input_destroy(spx_input_t *A)
{
  /* Check validity of input argument */
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid input handle");
    return SPX_FAILURE;
  }

  /* Free allocated memory of matrix handle */
  input_free_struct(A);

  return SPX_SUCCESS;
}

spx_matrix_t *spx_mat_tune(spx_input_t *input, ...)
{
  /* Check validity of input argument */
  if (!input) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid input matrix");
    return SPX_INVALID_MAT;
  }

  /* Check optional arguments */
  va_list ap;
  va_start(ap, input);
  spx_option_t option = va_arg(ap, spx_option_t);
  va_end(ap);

  /* Convert to CSX */
  spx_matrix_t *tuned = SPX_INVALID_MAT;
  spx_perm_t *permutation = SPX_INVALID_PERM;
  mat_alloc_struct(&tuned);
  tuned->nrows = input->nrows;
  tuned->ncols = input->ncols;
  tuned->nnz = input->nnz;
  if (input->type == 'C') {
    if (option == SPX_MAT_REORDER) {
      input->mat = ReorderCSR(input->mat, &permutation);
    }
    tuned->csx = TuneCSR(input->mat, &(tuned->symmetric));
  } else if (input->type == 'M') {
    if (option == SPX_MAT_REORDER) {
      input->mat = ReorderMMF(input->mat, &permutation);
    }
    tuned->csx = TuneMMF(input->mat, &(tuned->symmetric));
  }

  if (!tuned->csx) {
    mat_free_struct(tuned);
    SETERROR_0(SPX_ERR_TUNED_MAT);
    return SPX_INVALID_MAT;
  }

  tuned->permutation = permutation;
  permutation = SPX_INVALID_PERM;

  /* Create local buffers in case of CSX-Sym */
  if (tuned->symmetric) {
    spm_mt_t *spm_mt = (spm_mt_t *) tuned->csx;
    size_t nr_threads = spm_mt->nr_threads;
    spm_mt->local_buffers =
      (spx_vector_t **) malloc(nr_threads * sizeof(spx_vector_t *));

    size_t i;
#if SPX_USE_NUMA
    for (i = 1; i < nr_threads; i++) {
      int node = spm_mt->spm_threads[i].node;
      spm_mt->local_buffers[i] = VecCreateOnnode(tuned->nrows, node);
    }
#else
    for (i = 1; i < nr_threads; i++)
      spm_mt->local_buffers[i] = VecCreate(tuned->nrows);
#endif
  }

  CreatePool();
  return tuned;
}

spx_error_t spx_mat_get_entry(const spx_matrix_t *A, spx_index_t row,
                              spx_index_t column, spx_value_t *value, ...)
{
  /* Check optional arguments */
  va_list ap;
  va_start(ap, value);
  spx_option_t indexing = va_arg(ap, spx_option_t);
  va_end(ap);

  indexing = indexing - SPX_INDEX_ZERO_BASED;
  if (!check_indexing(indexing)) {
    indexing = 0;
  }

  /* Check validity of input argument */
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  if ((row - indexing) < 0 || (row - indexing) >= A->nrows || 
      (column - indexing) < 0 || (column - indexing) >= A->ncols) {
    SETERROR_0(SPX_OUT_OF_BOUNDS);
    return SPX_FAILURE;
  }

  /* Search for element */
  if (A->permutation != SPX_INVALID_PERM) {
    row = A->permutation[row - indexing] + 1;
    column = A->permutation[column - indexing] + 1;
  }

  /* Uses one-based indexing */
  int err = GetValue(A->csx, row + !indexing, column + !indexing, value);
  if (err) {
    SETERROR_0(SPX_ERR_ENTRY_NOT_FOUND);
    return SPX_FAILURE;
  }

  return SPX_SUCCESS;
}

spx_error_t spx_mat_set_entry(spx_matrix_t *A, spx_index_t row,
                              spx_index_t column, spx_value_t value, ...)
{
  /* Check optional arguments */
  va_list ap;
  va_start(ap, value);
  spx_option_t indexing = va_arg(ap, spx_option_t);
  va_end(ap);

  indexing = indexing - SPX_INDEX_ZERO_BASED;
  if (!check_indexing(indexing)) {
    indexing = 0;
  }

  /* Check validity of input arguments */
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  if ((row - indexing) < 0 || (row - indexing) >= A->nrows || 
      (column - indexing) < 0 || (column - indexing) >= A->ncols) {
    SETERROR_0(SPX_OUT_OF_BOUNDS);
    SETWARNING(SPX_WARN_ENTRY_NOT_SET);
    return SPX_FAILURE;
  }

  /* Set new value */
  if (A->permutation != SPX_INVALID_PERM) {
    row = A->permutation[row - indexing] + 1;
    column = A->permutation[column - indexing] + 1;
  }

  /* Uses one-based indexing */
  int err = SetValue(A->csx, row + !indexing, column + !indexing, value);
  if (err) {
    SETERROR_0(SPX_ERR_ENTRY_NOT_FOUND);
    return SPX_FAILURE;
  }

  return SPX_SUCCESS;
}

spx_error_t spx_mat_save(const spx_matrix_t *A, const char *filename)
{
  /* Check validity of input arguments */
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  if (!filename) {
    SETWARNING(SPX_WARN_CSXFILE);
  }

  /* Save the tuned matrix */
  SaveTuned(A->csx, filename, A->permutation);

  return SPX_SUCCESS;
}

spx_matrix_t *spx_mat_restore(const char *filename)
{
  /* Check validity of input argument */
  if (!filename) {
    SETERROR_0(SPX_ERR_FILE);
    return SPX_INVALID_MAT;
  }

  /* This is not a 100% safe check */
  if (access(filename, F_OK | R_OK) == -1) {
    SETERROR_0(SPX_ERR_FILE);
    return SPX_INVALID_MAT;
  }

  /* Load tuned matrix from file */
  spx_matrix_t *A = SPX_INVALID_MAT;
  mat_alloc_struct(&A);
  A->csx = LoadTuned(filename, &(A->nrows), &(A->ncols), &(A->nnz),
		     &(A->permutation));
  if (!A->csx) {
    SETERROR_0(SPX_ERR_TUNED_MAT);
    return SPX_INVALID_MAT;
  }

  CreatePool();
  return A;
}

spx_index_t spx_mat_get_nrows(const spx_matrix_t *A)
{
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  return  A->nrows;
}

spx_index_t spx_mat_get_ncols(const spx_matrix_t *A)
{
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  return A->ncols;
}

spx_index_t spx_mat_get_nnz(const spx_matrix_t *A)
{
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  return  A->nnz;
}

spx_partition_t *spx_mat_get_partition(const spx_matrix_t *A)
{
  spx_partition_t *ret = SPX_INVALID_PART;
  spm_mt_t *spm_mt = (spm_mt_t *) A->csx;
  part_alloc_struct(&ret);
  ret->row_start = (spx_index_t *) malloc(sizeof(*ret->row_start)*
					  spm_mt->nr_threads);
  ret->row_end = (spx_index_t *) malloc(sizeof(*ret->row_end)*
					spm_mt->nr_threads);
#if SPX_USE_NUMA
  ret->parts = (size_t *) malloc(sizeof(*ret->parts)*spm_mt->nr_threads);
  ret->nodes = (int *) malloc(sizeof(*ret->nodes)*spm_mt->nr_threads);
  ret->affinity = (int *) malloc(sizeof(*ret->affinity)*spm_mt->nr_threads);
  ret->nr_partitions = spm_mt->nr_threads;
#endif

  size_t i;
  for (i = 0; i < spm_mt->nr_threads; i++) {
    spm_mt_thread_t *spm = spm_mt->spm_threads + i;
#if SPX_USE_NUMA
    ret->parts[i] = spm->nr_rows * sizeof(spx_value_t);
    ret->nodes[i] = spm->node;
    ret->affinity[i] = spm->cpu;
#endif
    ret->row_start[i] = spm->row_start;
    ret->row_end[i] = spm->row_start + spm->nr_rows;
  }

  return ret;
}

spx_index_t *spx_partition_get_rs(const spx_partition_t *p)
{
  if (!p) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid partition handle");
    return NULL;
  }

  return p->row_start;
}

spx_index_t *spx_partition_get_re(const spx_partition_t *p)
{
  if (!p) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid partition handle");
    return NULL;
  }

  return p->row_end;
}

spx_perm_t *spx_mat_get_perm(const spx_matrix_t *A)
{
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_INVALID_PERM;
  }

  if (!(A->permutation)) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "a permutation is not available");
    return SPX_INVALID_PERM;
  }

  return A->permutation;
}

spx_error_t spx_matvec_mult(spx_value_t alpha, const spx_matrix_t *A, 
                            const spx_vector_t *x, spx_vector_t *y)
{
  /* Check validity of input arguments */
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  if (!x) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid vector x");
    return SPX_FAILURE;
  }

  if (!y) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid vector y");
    return SPX_FAILURE;
  }

  /* Check compatibility between matrix and vector dimensions */
  if (!check_vec_dim(x, A->ncols) && !check_vec_dim(y, A->nrows)) {
    SETERROR_0(SPX_ERR_DIM);
    return SPX_FAILURE;
  }

  /* Compute kernel */
  if (!A->symmetric) {
    MatVecMult(A->csx, x, alpha, y);
  } else {
    MatVecMult_sym(A->csx, x, alpha, y);
  }

  return SPX_SUCCESS;
}

spx_error_t spx_matvec_kernel(spx_value_t alpha, const spx_matrix_t *A, 
                              const spx_vector_t *x, spx_value_t beta,
                              spx_vector_t *y)
{
  /* Check validity of input arguments */
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  if (!x) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid vector x");
    return SPX_FAILURE;
  }

  if (!y) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid vector y");
    return SPX_FAILURE;
  }

  /* Check compatibility between matrix and vector dimensions */
  if (!check_vec_dim(x, A->ncols) && !check_vec_dim(y, A->nrows)) {
    SETERROR_0(SPX_ERR_DIM);
    return SPX_FAILURE;
  }

  /* Compute kernel */
  if (!A->symmetric) {
    MatVecKernel(A->csx, x, alpha, y, beta);
  } else {
    MatVecKernel_sym(A->csx, x, alpha, y, beta);
  }

  return SPX_SUCCESS;
}

spx_error_t spx_matvec_kernel_csr(spx_matrix_t **A, 
                                  spx_index_t nrows, spx_index_t ncols,
                                  const spx_index_t *rowptr,
                                  const spx_index_t *colind, 
                                  const spx_value_t *values,
                                  spx_value_t alpha, const spx_vector_t *x, 
                                  spx_value_t beta, spx_vector_t *y)
{
  /* Check validity of input arguments */
  if (!x) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid vector x");
    return SPX_FAILURE;
  }

  if (!y) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid vector y");
    return SPX_FAILURE;
  }

  if (!*A) {
    if (!check_mat_dim(nrows) || !check_mat_dim(ncols)) {
      SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix dimensions");
      return SPX_FAILURE;
    }

    if (!rowptr) {
      SETERROR_1(SPX_ERR_ARG_INVALID, "invalid rowptr argument");
      return SPX_FAILURE;
    }

    if (!colind) {
      SETERROR_1(SPX_ERR_ARG_INVALID, "invalid colind argument");
      return SPX_FAILURE;
    }

    if (!values) {
      SETERROR_1(SPX_ERR_ARG_INVALID, "invalid values argument");
      return SPX_FAILURE;
    }

    /* Assumes zero-based indexing */
    spx_input_t *input = spx_input_load_csr(rowptr, colind, values, nrows,
					    ncols, SPX_INDEX_ZERO_BASED);
    *A = SPX_INVALID_MAT;
    *A = spx_mat_tune(input);
    spx_input_destroy(input);
  }

  spx_matvec_kernel(alpha, *A, x, beta, y);

  return SPX_SUCCESS;
}

spx_error_t spx_mat_destroy(spx_matrix_t *A)
{
  /* Check validity of input argument */
  if (!A) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid matrix handle");
    return SPX_FAILURE;
  }

  /* Free allocated memory of matrix handle */
  mat_free_struct(A);

  return SPX_SUCCESS;
}

spx_partition_t *spx_partition_csr(const spx_index_t *rowptr,
                                   spx_index_t nr_rows, size_t nr_threads)
{
  spx_partition_t *ret = SPX_INVALID_PART;
  part_alloc_struct(&ret);
  ret->row_start = (spx_index_t *) malloc(sizeof(*ret->row_start)*nr_threads);
  ret->row_end = (spx_index_t *) malloc(sizeof(*ret->row_end)*nr_threads);
#if SPX_USE_NUMA
  ret->parts = (size_t *) malloc(sizeof(*ret->parts)*nr_threads);
  ret->nodes = (int *) malloc(sizeof(*ret->nodes)*nr_threads);
  ret->nr_partitions = nr_threads;
#endif

  // Compute the matrix splits.
  size_t nnz_per_split = (rowptr[nr_rows] - 1) / nr_threads;
  size_t curr_nnz = 0;
  size_t row_start = 0;
  size_t split_cnt = 0;
  spx_index_t i;

  ret->row_start[0] = row_start;
  for (i = 0; i < nr_rows; i++) {
    curr_nnz += rowptr[i+1] - rowptr[i];
    if (curr_nnz >= nnz_per_split) {
#if SPX_USE_NUMA
      ret->parts[split_cnt] = (i + 1 - row_start) * sizeof(spx_value_t);
#endif
      ret->row_end[split_cnt] = i + 1;
      row_start = i + 1;
      curr_nnz = 0;
      ++split_cnt;
      if (split_cnt < nr_threads)
	ret->row_start[split_cnt] = row_start;
    }
  }

  // Fill the last split.
  if (curr_nnz < nnz_per_split && split_cnt < nr_threads) {
#if SPX_USE_NUMA
    ret->parts[split_cnt] = (i + 1 - row_start) * sizeof(spx_value_t);
#endif
    ret->row_end[split_cnt] = i + 1;
  }

#if SPX_USE_NUMA
  GetNodes(ret->nodes);
#endif
  return ret;
}

spx_error_t spx_partition_destroy(spx_partition_t *p)
{
  /* Check validity of input argument */
  if (!p) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid partition handle");
    return SPX_FAILURE;
  }

  /* Free allocated memory of matrix handle */
  part_free_struct(p);

  return SPX_SUCCESS;
}

void spx_option_set(const char *option, const char *value)
{
  SetPropertyByMnemonic(option, value);
}

void spx_options_set_from_env()
{
  SetPropertiesFromEnv();
}

spx_vector_t *spx_vec_create(size_t size, const spx_partition_t *p)
{
  spx_vector_t *v = SPX_INVALID_VEC;

  if (p == SPX_INVALID_PART) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid partition handle");
    return SPX_INVALID_VEC;
  }

#if SPX_USE_NUMA
  v = VecCreateInterleaved(size, p->nr_partitions, p->parts, p->nodes,
			   p->row_start, p->row_end);
#else
  v = VecCreate(size);
#endif
  return v;
}

spx_vector_t *spx_vec_create_from_buff(spx_value_t *buff, spx_value_t **tuned,
                                       size_t size, const spx_partition_t *p,
                                       spx_vecmode_t mode)
{
  /* Check validity of input arguments */
  if (!buff) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid buffer");
    return SPX_INVALID_VEC;
  }

  if (!check_vecmode(mode)) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid vector mode");
    return SPX_INVALID_VEC;
  }

  if (p == SPX_INVALID_PART && mode == SPX_VEC_TUNE) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid partition handle");
    return SPX_INVALID_VEC;
  }

  spx_vector_t *v = SPX_INVALID_VEC;
  if (mode == SPX_VEC_TUNE) {
    v = VecCreateFromBuff(buff, size, p->nr_partitions, p->affinity,
			  p->row_start, p->row_end, mode);
  } else {
    v = VecCreateFromBuff(buff, size, -1, NULL, NULL, NULL, mode);
  }

#if SPX_USE_NUMA
  if (tuned)
    *tuned = v->elements;
#else
  if (tuned)
    *tuned = buff;
#endif

  return v;
}

spx_vector_t *spx_vec_create_random(size_t size, const spx_partition_t *p)
{
  spx_vector_t *v = SPX_INVALID_VEC;

  if (p == SPX_INVALID_PART) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid partition handle");
    return SPX_INVALID_VEC;
  }

#if SPX_USE_NUMA
  v = VecCreateRandomInterleaved(size, p->nr_partitions, p->parts,
				 p->nodes, p->row_start, p->row_end);
#else
  v = VecCreateRandom(size);
#endif
  return v;
}

void spx_vec_init(spx_vector_t *v, spx_value_t val)
{
  VecInit(v, val);
}

void spx_vec_init_part(spx_vector_t *v, spx_value_t val, spx_index_t start,
                       spx_index_t end)
{
  VecInitPart(v, val, start, end);
}

void spx_vec_init_rand_range(spx_vector_t *v, spx_value_t max, spx_value_t min)
{
  VecInitRandRange(v, max, min);
}

spx_error_t spx_vec_set_entry(spx_vector_t *v, spx_index_t idx, spx_value_t val,
                              ...)
{
  if (idx <= 0  || idx > v->size) {
    SETERROR_0(SPX_OUT_OF_BOUNDS);
    SETWARNING(SPX_WARN_ENTRY_NOT_SET);
    return SPX_FAILURE;
  }

  /* Check optional arguments */
  va_list ap;
  va_start(ap, val);
  spx_option_t indexing = va_arg(ap, spx_option_t);
  va_end(ap);

  indexing = indexing - SPX_INDEX_ZERO_BASED;
  if (!check_indexing(indexing)) {
    indexing = 0;
  }

  /* Uses one-based indexing */
  VecSetEntry(v, idx + !indexing, val);

  return SPX_SUCCESS;
}

void spx_vec_scale(spx_vector_t *v1, spx_vector_t *v2, spx_value_t num)
{
  VecScale(v1, v2, num);
}

void spx_vec_scale_add(spx_vector_t *v1, spx_vector_t *v2, spx_vector_t *v3, 
                       spx_value_t num)
{
  VecScaleAdd(v1, v2, v3, num);
}

void spx_vec_scale_add_part(spx_vector_t *v1, spx_vector_t *v2,
                            spx_vector_t *v3, spx_value_t num,
                            spx_index_t start, spx_index_t end)
{
  VecScaleAddPart(v1, v2, v3, num, start, end);
}

void spx_vec_add(spx_vector_t *v1, spx_vector_t *v2, spx_vector_t *v3)
{
  VecAdd(v1, v2, v3);
}

void spx_vec_add_part(spx_vector_t *v1, spx_vector_t *v2, spx_vector_t *v3,
                      spx_index_t start, spx_index_t end)
{
  VecAddPart(v1, v2, v3, start, end);
}

void spx_vec_sub(spx_vector_t *v1, spx_vector_t *v2, spx_vector_t *v3)
{
  VecSub(v1, v2, v3);
}

void spx_vec_sub_part(spx_vector_t *v1, spx_vector_t *v2, spx_vector_t *v3,
                      spx_index_t start, spx_index_t end)
{
  VecSubPart(v1, v2, v3, start, end);
}

spx_value_t spx_vec_mul(const spx_vector_t *v1, const spx_vector_t *v2)
{
  spx_value_t value = VecMult(v1, v2);
  return value;
}

spx_value_t spx_vec_mul_part(const spx_vector_t *v1, const spx_vector_t *v2,
                             spx_index_t start, spx_index_t end)
{
  spx_value_t value = VecMultPart(v1, v2, start, end);
  return value;
}

spx_error_t spx_vec_reorder(spx_vector_t *v, spx_perm_t *p)
{
  size_t i;
  spx_vector_t *permuted_v = SPX_INVALID_VEC;

  if (p == SPX_INVALID_PERM) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid permutation");
    return SPX_FAILURE;
  }

  permuted_v = VecCreate(v->size);
  for (i = 0; i < v->size; i++) {
    permuted_v->elements[p[i]] = v->elements[i];
  }

#if SPX_USE_NUMA
  VecCopy(permuted_v, v);
#else
  memcpy(v->elements, permuted_v->elements, v->size*sizeof(spx_value_t)); 
#endif
  spx_vec_destroy(permuted_v);

  return SPX_SUCCESS;
}

spx_error_t spx_vec_inv_reorder(spx_vector_t *v, spx_perm_t *p)
{
  size_t i;
  spx_vector_t *permuted_v = SPX_INVALID_VEC;

  if (p == SPX_INVALID_PERM) {
    SETERROR_1(SPX_ERR_ARG_INVALID, "invalid permutation");
    return SPX_FAILURE;
  }

  permuted_v = VecCreate(v->size);
  for (i = 0; i < v->size; i++) {
    permuted_v->elements[i] = v->elements[p[i]];
  }

#if SPX_USE_NUMA
  VecCopy(permuted_v, v);
#else
  memcpy(v->elements, permuted_v->elements, v->size*sizeof(spx_value_t)); 
#endif
  spx_vec_destroy(permuted_v);

  return SPX_SUCCESS;
}

void spx_vec_copy(const spx_vector_t *v1, spx_vector_t *v2)
{
  VecCopy(v1, v2);
}

int spx_vec_compare(const spx_vector_t *v1, const spx_vector_t *v2)
{
  return VecCompare(v1, v2);
}

void spx_vec_print(const spx_vector_t *v)
{
  VecPrint(v);
}

void spx_vec_destroy(spx_vector_t *v)
{
  VecDestroy(v);
}
