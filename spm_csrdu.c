#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>

#include "dynarray.h"
#include "mmf.h"
#include "mt_lib.h"

#include "spm_csrdu.h"
#include "spm_mt.h"

#define MIN(x,y) (x < y ? x : y)

typedef struct rle {
	uint64_t val;
	uint64_t freq;
} rle_t;

static void delta_encode(uint64_t *input, uint64_t *deltas, uint64_t size)
{
	uint64_t i;
	uint64_t prev = input[0];
	deltas[0] = prev ;
	for (i=1; i<size; i++){
		uint64_t curr = input[i];
		deltas[i] = curr - prev;
		prev = curr;
	}
	//printf("prev=%lu input=%lu\n", deltas[0], input[0]);
}

static void rle_encode(uint64_t *in, uint64_t insize, rle_t *rles, uint64_t *rles_size)
{
	uint64_t rle_freq=1; // frequency of current value
	uint64_t rle_i=0;    // index of rle buffer
	rle_t *rle;
	uint64_t i;
	uint64_t prev = in[0];
	//printf("prev=%lu\n", prev);
	for (i=1; i<insize; i++){
		uint64_t curr = in[i];
		if (curr == prev){
			rle_freq++;
			continue;
		}

		rle = rles + rle_i++;
		rle->val = prev;
		rle->freq = rle_freq;
		//printf("rles[%lu] = (f:%lu v:%lu)\n", rle-rles, rle->freq, rle->val);
		rle_freq = 1;
		prev = curr;
	}

	rle = &rles[rle_i++];
	rle->val = in[insize-1];
	rle->freq = rle_freq;
	//printf("rles[%lu] = (f:%lu v:%lu)\n", rle-rles, rle->freq, rle->val);

	*rles_size = rle_i;
}

struct unit_state {
	uint64_t start, size; // unit start / size in the row
	uint64_t jmp;
	uint8_t ci_size;      // current size (type) of unit
	char new_row;         // new row existance
	uint64_t *deltas;     // array of deltas for current row
};

static struct stats {
	uint64_t units_de;
	uint64_t units_sp[SPM_CSRDU_CISIZE_NR];
} stats = {0};

static struct csrdu_st  {
	int sp_minlen;      // min length for a SPARSE unit (0->use maximum size)
	int de_minlen;      // min length for a DENSE unit (0->no dense units)
	uint64_t row_size;  // size of current row
	struct unit_state unit;
	dynarray_t *da_ctl;

	unsigned aligned:1;    // use aligned deltas
	unsigned jmp:1;        // use jumps
	unsigned verbose:1;
} state;

// verbose message
#define vmsg(fmt,args...) do { if (state.verbose){ printf(fmt, args); }} while(0)

// parameter defaults
#define DE_MINLEN_DEF 0
#define SP_MINLEN_DEF 0
#define ALIGNED_DEF 0
#define JMP_DEF 0
#define VERBOSE_DEF 1
static void set_params()
{
	char *e;
	e = getenv("CSRDU_DE_MINLEN");
	state.de_minlen = e ? atoi(e) : DE_MINLEN_DEF;
	e = getenv("CSRDU_SP_MINLEN");
	state.sp_minlen = e ? atoi(e) : SP_MINLEN_DEF;
	e = getenv("CSRDU_ALIGNED");
	state.aligned = e ? !!atoi(e) : ALIGNED_DEF;
	e = getenv("CSRDU_JMP");
	state.jmp = e ? !!atoi(e) : JMP_DEF;
	e = getenv("CSRDU_VERBOSE");
	state.verbose = e ? !!atoi(e) : VERBOSE_DEF;

	vmsg("csrdu_params: sp_minlen:%d de_minlen:%d aligned:%d jmp:%d\n",
              state.sp_minlen, state.de_minlen, state.aligned, state.jmp);
}

static void de_add_unit()
{
	uint8_t *ctl_flags = dynarray_alloc_nr(state.da_ctl, 2);
	uint8_t *ctl_size = ctl_flags + 1;

	struct unit_state *ust = &state.unit;

	*ctl_flags = 0;
	*ctl_size = (uint8_t)ust->size;
	if (ust->new_row){
		 spm_csrdu_fl_setnr(ctl_flags);
		 ust->new_row = 0;
	}

	//printf("de_add usize: %lu ctl offset: %lu\n", ust->size, dynarray_size(state.da_ctl));
	da_uc_put_ul(state.da_ctl, ust->deltas[ust->start]);
	ust->start += ust->size;
	ust->size = 0;
	ust->ci_size = SPM_CSRDU_CISIZE_U8;

	stats.units_de++;
}

static void sp_add_header(uint64_t usize, uint8_t ci_size, char *new_row_ptr)
{
	//printf("sp_add_header usize: %lu ctl offset: %lu\n", usize, dynarray_size(state.da_ctl));
	uint8_t *ctl_flags = dynarray_alloc_nr(state.da_ctl, 2);
	uint8_t *ctl_size = ctl_flags + 1;

	*ctl_flags = 0;
	spm_csrdu_fl_setsp(ctl_flags);
	spm_csrdu_fl_setcisize(ctl_flags, ci_size);
	*ctl_size = (uint8_t)usize;
	if (*new_row_ptr){
		 spm_csrdu_fl_setnr(ctl_flags);
		 *new_row_ptr = 0;
	}

	stats.units_sp[ci_size]++;
}

static void sp_add_body(uint64_t ustart, uint64_t usize, uint8_t ci_size, uint64_t *deltas)
{
	uint64_t *src;
	void *dst;

	uint64_t dsize = spm_csrdu_cisize_bytes(ci_size);

	 // do the alignment, even if you don't put any indices,
	 // since it makes the decompression simpler
	if (state.aligned)
		dynarray_align(state.da_ctl, dsize);
	if (!usize)
		return;

	dst = dynarray_alloc_nr(state.da_ctl, usize*dsize);
	src = deltas + ustart;

	/*
	printf("sp_add_body: adding deltas: ");
	int i;
	for (i=0; i<usize; i++){
		printf("%lu ", src[i]);
	}
	printf("\n");
	printf("dsize:%lu dst: %p src: %p usize: %lu ci_size:%u\n", dsize, dst, src, usize, ci_size);
	*/

	if (usize <= 0){
		return;
	}
	if (state.aligned){
		spm_csrdu_cisize_copy(dst, src, usize, ci_size);
	} else {
		spm_csrdu_cisize_copy_ua(dst, src, usize, ci_size);
	}
}

static void sp_add_unit()
{
	struct unit_state *ust = &state.unit;
	sp_add_header(ust->size, ust->ci_size, &ust->new_row);
	sp_add_body(ust->start, ust->size, ust->ci_size, ust->deltas);
	ust->start += ust->size;
	ust->size = 0;
	ust->ci_size = SPM_CSRDU_CISIZE_U8;
}

static void sp_jmp_add_unit()
{
	struct unit_state *ust = &state.unit;
	//printf("sp_jmp_add: ust->start: %lu ust->size: %lu state.row_size:%lu new_r:%d ctl_off:%lu\n", ust->start, ust->size, state.row_size, ust->new_row, dynarray_size(state.da_ctl));
	assert(ust->start + ust->size <= state.row_size);

	assert(ust->size > 0);
	uint64_t ustart = ust->start;
	uint64_t usize = ust->size;
	uint64_t ci_size = ust->ci_size;
	sp_add_header(usize, ci_size, &ust->new_row);
	da_uc_put_ul(state.da_ctl, ust->jmp);
	//printf("sp_jmp_add: [jmp] ust->start: %lu ust->size: %lu state.row_size:%lu new_r:%d ctl_off:%lu\n", ust->start, ust->size, state.row_size, ust->new_row, dynarray_size(state.da_ctl));
	sp_add_body(ustart +1, usize -1, ci_size, ust->deltas);
	ust->start += ust->size;
	ust->size = 0;
	ust->ci_size = SPM_CSRDU_CISIZE_U8;
}
typedef void void_fn_t(void);
static void handle_row(uint64_t *deltas, uint64_t deltas_size,
                       rle_t *rles, uint64_t rles_size)
{
	struct unit_state *ust = &state.unit;
	ust->start = 0;
	ust->size = 0;
	ust->ci_size = SPM_CSRDU_CISIZE_U8;
	ust->deltas = deltas;

	int sp_minlen = state.sp_minlen;
	int de_minlen = state.de_minlen;

	void_fn_t *sp_add = state.jmp ? sp_jmp_add_unit : sp_add_unit;

	// when state.jmp is true, then we have the following semantics
	rle_t *rle = rles;
	rle_t *rle_end = rles + rles_size;
	if (state.jmp){
		// first element is a jmp
		ust->jmp = ust->deltas[0];
		assert(rle->freq >= 1);
		if (rle->freq == 1){
			rle++;
		} else {
			rle->freq--;
		}
		ust->size = 1;
	}

	while (rle < rle_end) {
		//printf("rle->freq %lu rle->val %lu ust->size %lu\n", rle->freq, rle->val, ust->size);
		// check if this is a large enough dense unit
		if (de_minlen && (rle->val == 1) && (rle->freq >= (de_minlen-1))){
			// add previous sparse unit (if exists)
			if (ust->size > 1){
				// the next unit is dense, so there is always a jmp
				ust->size--;
				sp_add();
				ust->size = 1;
			}

			// add dense unit
			do {
				uint64_t s = MIN(rle->freq, SPM_CSRDU_SIZE_MAX - ust->size);
				ust->size += s;
				rle->freq -= s;
				de_add_unit();
			} while (rle->freq >= de_minlen);

			if (rle->freq == 0){
				rle++;
				if (rle == rle_end){
					break;
				}
			}

			if (state.jmp){
				rle->freq--;
				ust->jmp = ust->deltas[ust->start];
				ust->size = 1;
				if (rle == rle_end){
					break;
				}
				continue;
			}
		}

		// check if the ci_size changes with the new column index
		uint8_t new_ci_size = spm_csrdu_cisize(rle->val);
		if (new_ci_size > ust->ci_size) {
			// check the unit is large enough to be commited
			if (sp_minlen && (ust->size >= sp_minlen)){
				sp_add();
				assert(0);
			}
			ust->ci_size = new_ci_size;
		}


		// check if by adding this rle the usize gets > max unit size
		while (rle->freq + ust->size > SPM_CSRDU_SIZE_MAX) {
			rle->freq -= (SPM_CSRDU_SIZE_MAX - ust->size);
			ust->size = SPM_CSRDU_SIZE_MAX;
			sp_add();
			if (state.jmp){
				ust->jmp = ust->deltas[ust->start];
				ust->size = 1;
				rle->freq--;
			}
		}

		ust->size += rle->freq;
		rle++;
	}

	// add remaining unit
	if (ust->size){
		sp_add();
	}

	ust->new_row = 1;
}

void SPM_CSRDU_NAME(_destroy)(void *m)
{
	SPM_CSRDU_TYPE *csrdu = (SPM_CSRDU_TYPE *)m;
	free(csrdu->values);
	free(csrdu->ctl);
	free(csrdu);
}

uint64_t SPM_CSRDU_NAME(_size)(void *m)
{
	SPM_CSRDU_TYPE *csrdu = (SPM_CSRDU_TYPE *)m;
	uint64_t ret = csrdu->ctl_size;
	ret += csrdu->nnz*sizeof(ELEM_TYPE);
	return ret;
}

void *SPM_CSRDU_NAME(_init_mmf)(char *mmf_file,
                                uint64_t *nrows, uint64_t *ncols, uint64_t *nnz)
{
	SPM_CSRDU_TYPE *csrdu;
	csrdu = malloc(sizeof(SPM_CSRDU_TYPE));
	if (!csrdu){
		perror("malloc");
		exit(1);
	}

	FILE *mmf = mmf_init(mmf_file, nrows, ncols, nnz);
	csrdu->nnz = *nnz;
	csrdu->ncols = *ncols;
	csrdu->nrows= *nrows;
	csrdu->values = malloc(sizeof(ELEM_TYPE)*csrdu->nnz);
	if (!csrdu->values){
		perror("malloc");
		exit(1);
	}

	dynarray_t *da_cis = dynarray_create(sizeof(uint64_t), 512);
	dynarray_t *da_deltas = dynarray_create(sizeof(uint64_t), 512);
	dynarray_t *da_rles = dynarray_create(sizeof(rle_t), 512);
	state.da_ctl = dynarray_create(sizeof(uint8_t), 4096);

	set_params();

	uint64_t row, col, row_prev, val_i=0;
	double val;
	state.unit.new_row = 0; // first row is without flag
	row_prev = 0;
	for(;;){
		int ret = mmf_get_next(mmf, &row, &col, &val);
		if ((row != row_prev)|| !ret){
			uint64_t *cis = dynarray_get(da_cis, 0);
			uint64_t cis_size = dynarray_size(da_cis);
			uint64_t *deltas = dynarray_alloc_nr(da_deltas, cis_size);
			rle_t *rles = dynarray_alloc_nr(da_rles, cis_size);
			uint64_t rles_size;

			state.row_size = cis_size;
			delta_encode(cis, deltas, cis_size);
			rle_encode(deltas, cis_size, rles, &rles_size);
			//printf("--- handle row start (row=%lu,row_size=%lu)\n", row_prev, state.row_size);
			handle_row(deltas, cis_size, rles, rles_size);
			//printf("--- handle row end\n");

			if (!ret){
				break;
			}

			dynarray_dealloc_all(da_cis);
			dynarray_dealloc_all(da_deltas);
			dynarray_dealloc_all(da_rles);
			row_prev = row;
		}

		uint64_t *ci = dynarray_alloc(da_cis);
		*ci = col;
		csrdu->values[val_i++] = (ELEM_TYPE)val;
	}

	fclose(mmf);
	free(dynarray_destroy(da_cis));
	free(dynarray_destroy(da_deltas));
	free(dynarray_destroy(da_rles));
	assert(val_i == csrdu->nnz);

	csrdu->ctl_size = dynarray_size(state.da_ctl);
	vmsg("ctl_size: %lu \n", csrdu->ctl_size);
	vmsg("units:\tde:%-6lu  sp(8):%-6lu  sp(16):%-6lu  sp(32):%-6lu  sp(64):%-6lu\n",
	      stats.units_de, stats.units_sp[SPM_CSRDU_CISIZE_U8], stats.units_sp[SPM_CSRDU_CISIZE_U16], stats.units_sp[SPM_CSRDU_CISIZE_U32], stats.units_sp[SPM_CSRDU_CISIZE_U64]);
	csrdu->ctl = dynarray_destroy(state.da_ctl);
	return csrdu;
}

static spm_mt_t *partition_ctl(uint8_t *ctl, uint64_t  nnz, void *csrdu)
{
	uint8_t *uc = ctl;
	unsigned int nr_cpus, *cpus_affinity;
	spm_mt_t *spm_mt;
	spm_csrdu_mt_t *csrdu_mt_start;

	mt_get_options(&nr_cpus, &cpus_affinity);

	spm_mt = malloc(sizeof(spm_mt_t));
	if (!spm_mt){
		perror("malloc");
		exit(1);
	}

	spm_mt->nr_threads = nr_cpus;
	spm_mt->spm_threads = malloc(sizeof(spm_mt_thread_t)*nr_cpus);
	if (!spm_mt->spm_threads){
		perror("malloc");
		exit(1);
	}

	csrdu_mt_start = malloc(sizeof(spm_csrdu_mt_t)*nr_cpus);
	if (!csrdu_mt_start){
		perror("malloc");
		exit(1);
	}

	uint64_t elements_total=0, elements=0;
	uint64_t elements_limit = nnz / nr_cpus;
	uint64_t ctl_last_off=0;
	int csrdu_mt_idx=0, nr=0;
	uint64_t y_indx = 0;
	uint64_t y_last = 0;
	uint64_t values_nr=0;
	uint64_t last_nnz=0;

	for (;;) {
		uint8_t *uc_start = uc;
		uint8_t flags = u8_get(uc);
		uint8_t size = u8_get(uc);

		nr = spm_csrdu_fl_isnr(flags);
		if (nr){
			y_indx++;
		}

		//printf("elements:%lu elements_limit:%lu elements_total:%lu nnz:%lu\n", elements, elements_limit, elements_total, nnz);
		values_nr += size;
		if ( (nr && (elements >= elements_limit)) || (values_nr == nnz) ) {
			spm_mt_thread_t *spm_mt_thread = spm_mt->spm_threads + csrdu_mt_idx;
			spm_csrdu_mt_t *csrdu_mt = csrdu_mt_start + csrdu_mt_idx;

			spm_mt_thread->cpu = cpus_affinity[csrdu_mt_idx];
			spm_mt_thread->spm = csrdu_mt;

			csrdu_mt->csrdu = csrdu;
			csrdu_mt->val_start = last_nnz;
			if (values_nr != nnz){
				assert(nr);
				csrdu_mt->nnz = elements;
			} else {
				csrdu_mt->nnz = nnz - last_nnz;
			}
			last_nnz += csrdu_mt->nnz;
			csrdu_mt->ctl_start = ctl_last_off;
			csrdu_mt->row_start = y_last;
			spm_csrdu_fl_clearnr(ctl + ctl_last_off);

			//printf("row_start:%lu ctl_start:%lu nnz:%lu val_start:%lu\n", csrdu_mt->row_start, csrdu_mt->ctl_start, csrdu_mt->nnz, csrdu_mt->val_start);
			if ( ++csrdu_mt_idx == nr_cpus){
				break;
			}

			//ctl_last_off = csrdu_mt->ctl_end = (uc_start - ctl);
			y_last = y_indx;
			ctl_last_off = (uc_start - ctl);
			elements_total += elements;
			elements_limit = (nnz - elements_total) / (nr_cpus - csrdu_mt_idx);
			elements = 0;
		}
		assert(values_nr < nnz);
		elements += size;

		uint8_t unit = flags & SPM_CSRDU_FL_UNIT_MASK;
		if (unit == SPM_CSRDU_FL_UNIT_DENSE){
			uc_get_ul(uc);
			continue;
		}

		if (state.jmp){
			u8_get_ul(uc);
			size--;
		}

		#define ALIGN(buf,a) (void *) (((unsigned long) (buf) + (a-1)) & ~(a-1))
		#define ALIGN_UC(align) do { uc = ALIGN(uc, align); } while(0)

		switch (unit){

			case SPM_CSRDU_FL_UNIT_SP_U8:
			uc += sizeof(uint8_t)*(size);
			break;

			case SPM_CSRDU_FL_UNIT_SP_U16:
			if (state.aligned)
				ALIGN_UC(2);
			uc += sizeof(uint16_t)*(size);
			break;

			case SPM_CSRDU_FL_UNIT_SP_U32:
			if (state.aligned)
				ALIGN_UC(4);
			uc += sizeof(uint32_t)*(size);
			break;

			case SPM_CSRDU_FL_UNIT_SP_U64:
			if (state.aligned)
				ALIGN_UC(8);
			uc += sizeof(uint64_t)*(size);
			break;

			default:
			printf("Uknown flags: %u unit=%u\n", flags, flags & SPM_CSRDU_FL_UNIT_MASK);
			assert(0);
		}

		assert(elements_total < nnz);
	}

	free(cpus_affinity);
	return spm_mt;
}

void *SPM_CSRDU_NAME(_mt_init_mmf)(char *mmf_file,
                                   uint64_t *nrows, uint64_t *ncols, uint64_t *nnz)
{
	spm_mt_t *ret;

	SPM_CSRDU_TYPE *csrdu;
	csrdu = SPM_CSRDU_NAME(_init_mmf)(mmf_file, nrows, ncols, nnz);

	ret = partition_ctl(csrdu->ctl, csrdu->nnz, csrdu);
	return ret;
}

uint64_t SPM_CSRDU_NAME(_mt_size)(void *spm)
{
	spm_mt_t *spm_mt = (spm_mt_t *)spm;
	spm_mt_thread_t *spm_thread = spm_mt->spm_threads;
	spm_csrdu_mt_t *csrdu_mt = spm_thread->spm;
	return SPM_CSRDU_NAME(_size)(csrdu_mt->csrdu);
}

void SPM_CSRDU_NAME(_mt_destroy)(void *spm)
{
	spm_mt_t *spm_mt = (spm_mt_t *)spm;
	spm_mt_thread_t *spm_thread = spm_mt->spm_threads;
	spm_csrdu_mt_t *csrdu_mt = spm_thread->spm;
	SPM_CSRDU_NAME(_destroy)(csrdu_mt->csrdu);
	free(csrdu_mt);
	free(spm_thread);
	free(spm_mt);
}

#ifdef SPM_NUMA
#include "numa.h"

static int numa_node_from_cpu(int cpu)
{
	struct bitmask *bmp;
	int nnodes, node, ret;

	bmp = numa_allocate_cpumask();
	nnodes =  numa_num_configured_nodes();
	for (node = 0; node < nnodes; node++){
		numa_node_to_cpus(node, bmp);
		if (numa_bitmask_isbitset(bmp, cpu)){
			ret = node;
			goto end;
		}
	}
	ret = -1;
end:
	numa_bitmask_free(bmp);
	return ret;
}

void *SPM_CSRDU_NAME(_mt_numa_init_mmf)(char *mmf_file,
                                        uint64_t *nrows, uint64_t *ncols,
                                        uint64_t *nnz)
{
	spm_mt_t *spm_mt;
	int i;
	spm_mt = SPM_CSRDU_NAME(_mt_init_mmf)(mmf_file, nrows, ncols, nnz);

	if (numa_available() == -1){
		perror("numa not available");
		exit(1);
	}

	/* keep a reference to the original csrdu */
	SPM_CSRDU_TYPE *csrdu = ((spm_csrdu_mt_t *)spm_mt->spm_threads[0].spm)->csrdu;
	ELEM_TYPE *values = csrdu->values;
	uint8_t *ctl = csrdu->ctl;
	int nr_threads = spm_mt->nr_threads;
	for (i=0; i<nr_threads; i++){
		spm_mt_thread_t *spm_thread = spm_mt->spm_threads + i;
		spm_csrdu_mt_t *csrdu_mt = (spm_csrdu_mt_t *)spm_thread->spm;
		/* get numa node from cpu */
		int node = numa_node_from_cpu(spm_thread->cpu);
		/* allocate new space */
		SPM_CSRDU_TYPE *numa_csrdu = numa_alloc_onnode(sizeof(SPM_CSRDU_TYPE), node);
		if (!numa_csrdu){
			perror("numa_alloc_onnode");
			exit(1);
		}
		uint64_t ctl_start = csrdu_mt->ctl_start;
		uint64_t ctl_end = (i < nr_threads -1) ? (csrdu_mt+1)->ctl_start : csrdu->ctl_size;
		uint64_t ctl_size = ctl_end - ctl_start;
		uint64_t row_start = csrdu_mt->row_start;
		uint64_t row_end = (i < nr_threads -1) ? (csrdu_mt+1)->row_start : csrdu->nrows;
		uint64_t nnz = csrdu_mt->nnz;
		numa_csrdu->values = numa_alloc_onnode(sizeof(ELEM_TYPE)*nnz, node);
		/*
		 * Note that the newly allocated ctl should have the same alignment with
		 * the previous part
		 */
		unsigned long align = (unsigned long)(ctl + ctl_start) & (8UL-1UL);
		numa_csrdu->ctl = numa_alloc_onnode(ctl_size + align, node);
		if (!numa_csrdu->values || !numa_csrdu->ctl){
			perror("numa_alloc_onnode");
			exit(1);
		}
		/* copy data */
		memcpy(numa_csrdu->values, values, sizeof(ELEM_TYPE)*nnz);
		values += nnz;
		memcpy(numa_csrdu->ctl + align, ctl + ctl_start, ctl_size);
		/* make the swap */
		numa_csrdu->nnz = nnz;
		numa_csrdu->ncols = csrdu->ncols;
		numa_csrdu->nrows = row_end - row_start;
		numa_csrdu->ctl_size = ctl_size;
		csrdu_mt->csrdu = numa_csrdu;
		csrdu_mt->ctl_start = align;
	}
	SPM_CSRDU_NAME(_destroy)(csrdu);

	return spm_mt;
}

void SPM_CSRDU_NAME(_mt_numa_destroy)(void *spm)
{
	spm_mt_t *spm_mt = (spm_mt_t *)spm;
	int i;
	for (i=0; i<spm_mt->nr_threads; i++){
		spm_mt_thread_t *spm_thread = spm_mt->spm_threads + i;
		spm_csrdu_mt_t *csrdu_mt = (spm_csrdu_mt_t *)spm_thread->spm;
		SPM_CSRDU_TYPE *csrdu = csrdu_mt->csrdu;
		numa_free(csrdu->values, csrdu->nnz);
		numa_free(csrdu->ctl, csrdu->ctl_size);
		numa_free(csrdu, sizeof(SPM_CSRDU_TYPE));
	}
	free(spm_mt->spm_threads);
	free(spm_mt);
}

uint64_t SPM_CSRDU_NAME(_mt_numa_size)(void *spm)
{
	spm_mt_t *spm_mt = (spm_mt_t *)spm;
	int i;
	uint64_t ret=0;
	for (i=0; i<spm_mt->nr_threads; i++){
		spm_mt_thread_t *spm_thread = spm_mt->spm_threads + i;
		spm_csrdu_mt_t *csrdu_mt = (spm_csrdu_mt_t *)spm_thread->spm;
		SPM_CSRDU_TYPE *csrdu = csrdu_mt->csrdu;
		ret += csrdu->nnz;
	}
	return ret;
}

#endif /* SPM_NUMA */
