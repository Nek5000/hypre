/*BHEADER**********************************************************************
 * (c) 1999   The Regents of the University of California
 *
 * See the file COPYRIGHT_and_DISCLAIMER for a complete copyright
 * notice, contact person, and disclaimer.
 *
 * $Revision$
 *********************************************************************EHEADER*/
/******************************************************************************
 *
 * DiagScale - Diagonal scaling.  Provide  
 * to 
 *
 *****************************************************************************/

#include <stdlib.h>
#include <assert.h>
#include "math.h"
#include "mpi.h"
#include "Hash.h"
#include "Matrix.h"
#include "RowPatt.h"
#include "DiagScale.h"
#include "OrderStat.h"

#define MAX_NPES 1024
#define DIAG_VALS_TAG 222
#define DIAG_INDS_TAG 223

#define DIAGSCALE_MAXLEN 50021  /* a prime number */

#define ABS(x) (((x)<0)?(-(x)):(x))

/*--------------------------------------------------------------------------
 * DIAGSCALE_EXIT - Print message, flush all output streams, return -1 to 
 * operating system, and exit to operating system.  Used internally only.
 *--------------------------------------------------------------------------*/

#define DIAGSCALE_EXIT \
{  printf("Exiting...\n"); \
   fflush(NULL); \
   MPI_Abort(MPI_COMM_WORLD, -1); \
}

/*--------------------------------------------------------------------------
 * ExchangeDiagEntries - Given a list of indices of diagonal entries required
 * by this processor, "reqind" of length "reqlen", return a list of 
 * corresponding diagonal entries, "diags".  Used internally only by
 * DiagScaleCreate.
 *
 * comm   - MPI communicator (input)
 * mat    - matrix used to map row and column numbers to processors (input)
 * reqlen - length of request list (input)
 * reqind - list of indices (input)
 * diags  - corresponding list of diagonal entries (output)
 * num_requests - number of requests (output)
 * requests - request handles, used to check that all responses are back 
 *            (output)
 *--------------------------------------------------------------------------*/

static void ExchangeDiagEntries(MPI_Comm comm, Matrix *mat, int reqlen, 
  int *reqind, double *diags, int *num_requests, MPI_Request *requests)
{
    MPI_Request request;
    int i, j, this_pe;

    shell_sort(reqlen, reqind);

    *num_requests = 0;

    for (i=0; i<reqlen; i=j) /* j is set below */
    {
        /* The processor that owns the row with index reqind[i] */
        this_pe = MatrixRowPe(mat, reqind[i]);

        /* Figure out other rows we need from this_pe */
        for (j=i+1; j<reqlen; j++)
        {
            /* if row is on different pe */
            if (reqind[j] < mat->beg_rows[this_pe] ||
                reqind[j] > mat->end_rows[this_pe])
                   break;
        }

        /* Post receive for diagonal values */
        MPI_Irecv(&diags[i], j-i, MPI_DOUBLE, this_pe, DIAG_VALS_TAG, 
	    comm, &requests[*num_requests]);

        /* Request rows in reqind[i..j-1] */
        MPI_Isend(&reqind[i], j-i, MPI_INT, this_pe, DIAG_INDS_TAG,
            comm, &request);
        MPI_Request_free(&request);

        (*num_requests)++;
    }
}

/*--------------------------------------------------------------------------
 * ExchangeDiagEntriesServer - Receive requests for diagonal entries and
 * send replies.  Used internally only by DiagScaleCreate.
 * 
 * comm   - MPI communicator (input)
 * mat    - matrix used to map row and column numbers to processors (input)
 * local_diags - local diagonal entries (input)
 * len - maximum length of incoming message, should be set to total 
 *       number of required indices (input)
 * num_requests - number of requests to be received (input)
 *--------------------------------------------------------------------------*/

static void ExchangeDiagEntriesServer(MPI_Comm comm, Matrix *mat, 
  double *local_diags, int len, int num_requests)
{
    MPI_Request request;
    MPI_Status status;
    int *recvbuf;
    double *sendbuf;
    int i, j, source, count;

    /* recvbuf contains requested indices */
    /* sendbuf contains corresponding diagonal entries */

    recvbuf = (int *)    malloc(len * sizeof(int));
    sendbuf = (double *) malloc(len * sizeof(double));

    /* Use this request handle to check that the send buffer is clear */
    request = MPI_REQUEST_NULL;

    for (i=0; i<num_requests; i++)
    {
        MPI_Recv(recvbuf, len, MPI_INT, MPI_ANY_SOURCE, 
	    DIAG_INDS_TAG, comm, &status);
        source = status.MPI_SOURCE;

        MPI_Get_count(&status, MPI_INT, &count);

	/* Wait until send buffer is clear */
	MPI_Wait(&request, &status);

        for (j=0; j<count; j++)
	    sendbuf[j] = local_diags[recvbuf[j] - mat->beg_row];

	/* Use ready-mode send, since receives already posted */
	MPI_Irsend(sendbuf, count, MPI_DOUBLE, source, 
	    DIAG_VALS_TAG, comm, &request);
    }

    /* Wait for final send to complete before freeing sendbuf */
    MPI_Wait(&request, &status);

    free(recvbuf);
    free(sendbuf);
}

/*--------------------------------------------------------------------------
 * DiagScaleCreate - Return (a pointer to) a diagonal scaling object.
 *--------------------------------------------------------------------------*/

DiagScale *DiagScaleCreate(Matrix *mat)
{
    MPI_Request requests[MAX_NPES];
    MPI_Status  statuses[MAX_NPES];
    int row, i, j, num_requests;
    int len, *ind;
    double *val;
    RowPatt *patt;
    double *diags;
    int inserted;

    DiagScale *p = (DiagScale *) malloc(sizeof(DiagScale));

    /* Storage for local diagonal entries */
    p->local_diags = (double *) 
        malloc((mat->end_row - mat->beg_row + 1) * sizeof(double));

    /* Extract the local diagonal entries and 
       merge pattern of all local rows to determine what we need */

    patt = RowPattCreate(DIAGSCALE_MAXLEN);

    for (row=mat->beg_row; row<=mat->end_row; row++)
    {
        MatrixGetRow(mat, row, &len, &ind, &val);
	RowPattMergeExt(patt, len, ind, mat->beg_row, mat->end_row);

        p->local_diags[row - mat->beg_row] = 1.0; /* in case no diag entry */

        for (j=0; j<len; j++)
        {
            if (ind[j] == row)
            {
                p->local_diags[row - mat->beg_row] = val[j];
                break;
            }
        }
    }

    /* Get the list of diagonal indices that we need;
       (ind is an array stored in patt, so do not destroy patt yet) */
    RowPattGet(patt, &len, &ind);

    /* buffer for receiving diagonal values from other processors */
    diags = (double *) malloc(len * sizeof(double));

    ExchangeDiagEntries(mat->comm, mat, len, ind, diags, &num_requests, 
        requests);

    ExchangeDiagEntriesServer(mat->comm, mat, p->local_diags, len, 
        num_requests);

    /* Wait for all replies */
    MPI_Waitall(num_requests, requests, statuses);

    /* Storage and indexing mechanism for external diagonal entries */
    p->ext_diags = (double *) malloc((2*len+1) * sizeof(double));
    p->hash  = HashCreate(2*len+1);

    /* loop over replies buffer and put into hash table */
    for (i=0; i<len; i++)
    {
        j = HashInsert(p->hash, ind[i], &inserted);
	p->ext_diags[j] = diags[i];
    }

    RowPattDestroy(patt);
    free(diags);

    return p;
}

/*--------------------------------------------------------------------------
 * DiagScaleDestroy - Destroy a diagonal scale object.
 *--------------------------------------------------------------------------*/

void DiagScaleDestroy(DiagScale *p)
{
    HashDestroy(p->hash);

    free(p->local_diags);
    free(p->ext_diags);

    free(p);
}

/*--------------------------------------------------------------------------
 * DiagScaleGet -  Returns scale factor given a global index,
 * The factor is the reciprocal of the square root of the diagonal entry.
 *--------------------------------------------------------------------------*/

double DiagScaleGet(DiagScale *p, Matrix *mat, int global_index)
{
    int index;

    if (mat->beg_row <= global_index && global_index <= mat->end_row)
        return 1.0 / sqrt(ABS(p->local_diags[global_index - mat->beg_row]));

    index = HashLookup(p->hash, global_index);

    return 1.0 / sqrt(ABS(p->ext_diags[index]));
}
