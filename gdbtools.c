#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* these are for debuging in GDB */
#include "allvars.h"

int GDB_particle_by_id(MyIDType id, int from) {
    int i;
    for(i = from; i < NumPart; i++) {
        if(P[i].ID == id) return i;
    }
    return -1;
}

int GDB_particle_by_type(int type, int from) {
    int i;
    for(i = from; i < NumPart; i++) {
        if(P[i].Type == type) return i;
    }
    return -1;
}

int GDB_particle_by_generation(int gen, int from) {
    int i;
    for(i = from; i < NumPart; i++) {
        if(P[i].Generation == gen) return i;
    }
    return -1;
}

char * GDB_particle_by_timebin(int bin) {
    int i;
    static char buf[1024];
    char tmp[1024];
    strcpy(buf, "");
    for(i = 0; i < NumPart; i++) {
        if(P[i].TimeBin == bin) {
            strcpy(tmp, buf);
            snprintf(buf, 1020, "%s %d", tmp, i);
        }
    }
    return buf;
}

