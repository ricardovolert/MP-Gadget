#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string.h>

#include "bigfile-mpi.h"
#include "mpsort.h"

#include "allvars.h"
#include "proto.h"
#include "petaio.h"
#include "domain.h"
#include "fof.h"
#include "mymalloc.h"
#include "endrun.h"

static void fof_write_header(BigFile * bf);
static void build_buffer_fof(BigArray * array, IOTableEntry * ent);

static void fof_return_particles();
static void fof_distribute_particles();

static int fof_cmp_argind(const void *p1, const void * p2) {
    const int * i1 = p1;
    const int * i2 = p2;
    return (P[*i1].GrNr > P[*i2].GrNr) - (P[*i1].GrNr < P[*i2].GrNr);
}

static void fof_radix_Group_GrNr(const void * a, void * radix, void * arg);
static void fof_radix_Group_GrNr(const void * a, void * radix, void * arg) {
    uint64_t * u = (uint64_t *) radix;
    struct BaseGroup * f = (struct BaseGroup*) a;
    u[0] = f->GrNr;
}

void fof_save_particles(int num) {
    char fname[4096];
    sprintf(fname, "%s/PIG_%03d", All.OutputDir, num);
    message(0, "saving particle in group into %s\n", fname);

    /* sort the groups according to group-number */
    mpsort_mpi(Group, Ngroups, sizeof(struct Group), 
            fof_radix_Group_GrNr, 8, NULL, MPI_COMM_WORLD);

    walltime_measure("/FOF/IO/Misc");
    fof_distribute_particles();
    walltime_measure("/FOF/IO/Distribute");
    
    int * argind = mymalloc("argind", sizeof(int) * NumPart);
    int NumPIG = 0;
    int i;
    for(i = 0; i < NumPart; i ++) {
        int j = NumPIG;
        if(P[i].GrNr >= 0) {
            argind[j] = i;
            NumPIG ++;
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    int NumPIGMax, NumPIGMin;
    MPI_Allreduce(&NumPIG, &NumPIGMax, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&NumPIG, &NumPIGMin, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    message(0, "NumPIGmin=%d NumPIGmax=%d\n", NumPIGMin, NumPIGMax);

    qsort(argind, NumPIG, sizeof(int), fof_cmp_argind);
    walltime_measure("/FOF/IO/argind");
    MPI_Barrier(MPI_COMM_WORLD);
    message(0, "argind sorted\n");

    BigFile bf = {0};
    if(0 != big_file_mpi_create(&bf, fname, MPI_COMM_WORLD)) {
        endrun(0, "Failed to open IC from %s\n", fname);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    fof_write_header(&bf); 

    for(i = 0; i < IOTable.used; i ++) {
        /* only process the particle blocks */
        char blockname[128];
        int ptype = IOTable.ent[i].ptype;
        BigArray array = {0};
        if(ptype < 6 && ptype >= 0) {
            sprintf(blockname, "%d/%s", ptype, IOTable.ent[i].name);
            petaio_build_buffer(&array, &IOTable.ent[i], argind, NumPIG);
        } else 
        if(ptype == PTYPE_FOF_GROUP) {
            sprintf(blockname, "FOFGroups/%s", IOTable.ent[i].name);
            build_buffer_fof(&array, &IOTable.ent[i]);
        } else {
            abort();
        }
        message(0, "Writing Block %s\n", blockname);

        petaio_save_block(&bf, blockname, &array, All.NumPartPerFile, All.NumWriters);
        petaio_destroy_buffer(&array);
    }

    myfree(argind); 
    big_file_mpi_close(&bf, MPI_COMM_WORLD);
    walltime_measure("/FOF/IO/Write");

    fof_return_particles();
    walltime_measure("/FOF/IO/Return");
}

struct PartIndex {
    uint64_t origin;
    union {
        int64_t sortKey;
        int targetTask;
    };
};

static int fof_sorted_layout(int i) {
    return P[i].targettask;
}
static int fof_origin_layout(int i) {
    return P[i].origintask;
}
static void fof_radix_sortkey(const void * c1, void * out, void * arg) {
    uint64_t * u = out;
    const struct PartIndex * pi = c1;
    *u = pi->sortKey;
}
static void fof_radix_origin(const void * c1, void * out, void * arg) {
    uint64_t * u = out;
    const struct PartIndex * pi = c1;
    *u = pi->origin;
}
#if 0
/*Unused functions*/
static int fof_select_particle(int i) {
    return P[i].GrNr > 0;
}
static int fof_cmp_sortkey(const void * c1, const void * c2) {
    const struct PartIndex * p1 = c1;
    const struct PartIndex * p2 = c2;
    return (p1->sortKey > p2->sortKey) - (p1->sortKey < p2->sortKey);
}
static int fof_cmp_origin(const void * c1, const void * c2) {
    const struct PartIndex * p1 = c1;
    const struct PartIndex * p2 = c2;
    return (p1->origin > p2->origin) - (p1->origin < p2->origin);
}
#endif

static void fof_distribute_particles() {
    int i;
    struct PartIndex * pi = mymalloc("PartIndex", sizeof(struct PartIndex) * NumPart);

    int64_t NpigLocal = 0;
    int GrNrMax = -1;	/* will mark particles that are not in any group */
    int GrNrMaxGlobal = 0;
    for(i = 0; i < NumPart; i ++) {
        int j = NpigLocal;
        if(P[i].GrNr < 0) continue;
        if(P[i].GrNr > GrNrMax) GrNrMax = P[i].GrNr;
/* Yu: found it! this shall be int64 */
        // pi[j].origin =  ThisTask * All.MaxPart + i;
        pi[j].origin = ((uint64_t) ThisTask) * All.MaxPart + i;
        pi[j].sortKey = P[i].GrNr;
        NpigLocal ++;
    }
    MPI_Allreduce(&GrNrMax, &GrNrMaxGlobal, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    message(0, "GrNrMax before exchange is %d\n", GrNrMaxGlobal);
    /* sort pi to decide targetTask */
    mpsort_mpi(pi, NpigLocal, sizeof(struct PartIndex), 
            fof_radix_sortkey, 8, NULL, MPI_COMM_WORLD);

#pragma omp parallel for
    for(i = 0; i < NumPart; i ++) {
        P[i].origintask = ThisTask;
        P[i].targettask = ThisTask; //P[i].ID % NTask; /* default target */
    }

    //int64_t Npig = count_sum(NpigLocal);
    //int64_t offsetLocal = count_to_offset(NpigLocal);

    //size_t chunksize = (Npig / NTask) + (Npig % NTask != 0);

    for(i = 0; i < NpigLocal; i ++) {
/* YU: A typo error here, should be IMIN, DMIN is for double but this should have tainted TargetTask,
   offset and chunksize are int  */
        //ptrdiff_t offset = offsetLocal + i;
        //pi[i].targetTask = IMIN(offset / chunksize, NTask - 1);
    /* YU: let's see if we keep the FOF particle load on the processes, IO would be faster
           (as at high z many ranks has no FOF), communication becomes sparse. */
        pi[i].targetTask = ThisTask;
    }
    /* return pi to the original processors */
    mpsort_mpi(pi, NpigLocal, sizeof(struct PartIndex), fof_radix_origin, 8, NULL, MPI_COMM_WORLD);
    for(i = 0; i < NpigLocal; i ++) {
        int index = pi[i].origin % All.MaxPart;
        P[index].targettask = pi[i].targetTask;
    }

    domain_exchange(fof_sorted_layout);

    myfree(pi);
    /* sort SPH and Others independently */

    GrNrMax = -1;
    for(i = 0; i < NumPart; i ++) {
        if(P[i].GrNr < 0) continue;
        if(P[i].GrNr > GrNrMax) GrNrMax = P[i].GrNr;
    }
    MPI_Allreduce(&GrNrMax, &GrNrMaxGlobal, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    message(0, "GrNrMax after exchange is %d\n", GrNrMaxGlobal);

}
static void fof_return_particles() {
    domain_exchange(fof_origin_layout);
}

static void build_buffer_fof(BigArray * array, IOTableEntry * ent) {

    int64_t npartLocal = Ngroups;

    petaio_alloc_buffer(array, ent, npartLocal);
    /* fill the buffer */
    char * p = array->data;
    int i;
    for(i = 0; i < Ngroups; i ++) {
        ent->getter(i, p);
        p += array->strides[0];
    }
}

static void fof_write_header(BigFile * bf) {
    BigBlock bh = {0};
    if(0 != big_file_mpi_create_block(bf, &bh, "Header", NULL, 0, 0, 0, MPI_COMM_WORLD)) {
        endrun(0, "Failed to create header\n");
    }
    int i;
    int k;
    int64_t npartLocal[6];
    int64_t npartTotal[6];

    for (k = 0; k < 6; k ++) {
        npartLocal[k] = 0;
    }
    for (i = 0; i < NumPart; i ++) {
        if(P[i].GrNr < 0) continue; /* skip those not in groups */
        npartLocal[P[i].Type] ++;
    }

    MPI_Allreduce(npartLocal, npartTotal, 6, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);
    
    big_block_set_attr(&bh, "NumPartInGroupTotal", npartTotal, "u8", 6);
    big_block_set_attr(&bh, "NumFOFGroupsTotal", &TotNgroups, "u8", 1);
    big_block_set_attr(&bh, "MassTable", All.MassTable, "f8", 6);
    big_block_set_attr(&bh, "Time", &All.Time, "f8", 1);
    big_block_set_attr(&bh, "BoxSize", &All.BoxSize, "f8", 1);
    big_block_set_attr(&bh, "OmegaLambda", &All.CP.OmegaLambda, "f8", 1);
    big_block_set_attr(&bh, "Omega0", &All.CP.Omega0, "f8", 1);
    big_block_set_attr(&bh, "HubbleParam", &All.CP.HubbleParam, "f8", 1);
    big_block_set_attr(&bh, "CMBTemperature", &All.CP.CMBTemperature, "f8", 1);
    big_block_set_attr(&bh, "OmegaBaryon", &All.CP.OmegaBaryon, "f8", 1);
    big_block_mpi_close(&bh, MPI_COMM_WORLD);
}

SIMPLE_PROPERTY(GroupID, Group[i].base.GrNr, uint32_t, 1)
SIMPLE_PROPERTY(MinID, Group[i].base.MinID, uint64_t, 1)
SIMPLE_PROPERTY(FirstPos, Group[i].base.FirstPos[0], float, 3)
SIMPLE_PROPERTY(MassCenterPosition, Group[i].CM[0], double, 3)
SIMPLE_PROPERTY(Imom, Group[i].Imom[0][0], float, 9)
SIMPLE_PROPERTY(Jmom, Group[i].Jmom[0], float, 3)
SIMPLE_PROPERTY(MassCenterVelocity, Group[i].Vel[0], float, 3)
SIMPLE_PROPERTY(Mass, Group[i].Mass, float, 1)
SIMPLE_PROPERTY(MassByType, Group[i].MassType[0], float, 6)
SIMPLE_PROPERTY(LengthByType, Group[i].LenType[0], uint32_t , 6)
#ifdef SFR
SIMPLE_PROPERTY(StarFormationRate, Group[i].Sfr, float, 1)
#endif
#ifdef BLACK_HOLES
SIMPLE_PROPERTY(BlackholeMass, Group[i].BH_Mass, float, 1)
SIMPLE_PROPERTY(BlackholeAccretionRate, Group[i].BH_Mdot, float, 1)
#endif

void fof_register_io_blocks() {
    IO_REG(GroupID, "u4", 1, PTYPE_FOF_GROUP);
    IO_REG(Mass, "f4", 1, PTYPE_FOF_GROUP);
    IO_REG(MassCenterPosition, "f8", 3, PTYPE_FOF_GROUP);
    IO_REG(FirstPos, "f4", 3, PTYPE_FOF_GROUP);
    IO_REG(MinID, "u8", 1, PTYPE_FOF_GROUP);
    IO_REG(Imom, "f4", 9, PTYPE_FOF_GROUP);
    IO_REG(Jmom, "f4", 3, PTYPE_FOF_GROUP);
    IO_REG(MassCenterVelocity, "f4", 3, PTYPE_FOF_GROUP);
    IO_REG(LengthByType, "u4", 6, PTYPE_FOF_GROUP);
    IO_REG(MassByType, "f4", 6, PTYPE_FOF_GROUP);
#ifdef SFR
    IO_REG(StarFormationRate, "f4", 1, PTYPE_FOF_GROUP);
#endif
#ifdef BLACK_HOLES
    IO_REG(BlackholeMass, "f4", 1, PTYPE_FOF_GROUP);
    IO_REG(BlackholeAccretionRate, "f4", 1, PTYPE_FOF_GROUP);
#endif
}
