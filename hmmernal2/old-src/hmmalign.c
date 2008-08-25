/* hmmalign.c
 * main() for aligning a set of sequences to an HMM
 *
 * SRE, Thu Dec 18 16:05:29 1997 [St. Louis]
 * SVN $Id: hmmalign.c 1382 2005-05-03 22:25:36Z eddy $
 */ 

#include "config.h"		/* compile-time configuration constants */
#include "squidconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "squid.h"		/* general sequence analysis library    */
#include "msa.h"		/* squid's multiple alignment i/o       */

#include "plan7.h"		/* plan 7 profile HMM structure         */
#include "structs.h"		/* data structures, macros, #define's   */
#include "funcs.h"		/* function declarations                */
#include "globals.h"		/* alphabet global variables            */


static char banner[] = "hmmalign - align sequences to an HMM profile";

static char usage[]  = "\
Usage: hmmalign [-options] <hmm file> <sequence file>\n\
Available options are:\n\
   -h     : help; print brief help on version and usage\n\
   -m     : only print symbols aligned to match states\n\
   -o <f> : save alignment in file <f>\n\
   -q     : quiet - suppress verbose banner\n\
";

static char experts[] = "\
   --informat <s>  : sequence file is in format <s>\n\
   --mapali <f>    : include alignment in file <f> using map in HMM\n\
   --oneline       : output Stockholm fmt with 1 line/seq, not interleaved\n\
   --outformat <s> : output alignment in format <s> [default: Stockholm]\n\
                       formats include: MSF, Clustal, Phylip, SELEX\n\
   --withali <f>   : include alignment to (fixed) alignment in file <f>\n\
\n";

static struct opt_s OPTIONS[] = {
  { "-h", TRUE, sqdARG_NONE   }, 
  { "-m", TRUE, sqdARG_NONE   } ,
  { "-o", TRUE, sqdARG_STRING },
  { "-q", TRUE, sqdARG_NONE   },
  { "--informat",  FALSE, sqdARG_STRING },
  { "--mapali",    FALSE, sqdARG_STRING },
  { "--oneline",   FALSE, sqdARG_NONE },
  { "--outformat", FALSE, sqdARG_STRING },
  { "--withali",   FALSE, sqdARG_STRING },
};
#define NOPTIONS (sizeof(OPTIONS) / sizeof(struct opt_s))

static void include_alignment(char *seqfile, struct plan7_s *hmm, int do_mapped,
			      char ***rseq, unsigned char ***dsq, SQINFO **sqinfo, 
			      struct p7trace_s ***tr, int *nseq);

int
main(int argc, char **argv) 
{
  char            *hmmfile;	/* file to read HMMs from                  */
  HMMFILE         *hmmfp;       /* opened hmmfile for reading              */
  struct plan7_s  *hmm;         /* HMM to align to                         */ 
  char            *seqfile;     /* file to read target sequence from       */ 
  int              format;	/* format of seqfile                       */
  char           **rseq;        /* raw, unaligned sequences                */ 
  SQINFO          *sqinfo;      /* info associated with sequences          */
  unsigned char  **dsq;         /* digitized raw sequences                 */
  int              nseq;        /* number of sequences                     */  
  float           *wgt;		/* weights to assign to alignment          */
  MSA             *msa;         /* alignment that's created                */    
  int              i;
  struct dpmatrix_s *mx;        /* growable DP matrix                      */
  struct p7trace_s **tr;        /* traces for aligned sequences            */

  char *optname;                /* name of option found by Getopt()         */
  char *optarg;                 /* argument found by Getopt()               */
  int   optind;                 /* index in argv[]                          */
  int   be_quiet;		/* TRUE to suppress verbose banner          */
  int   matchonly;		/* TRUE to show only match state syms       */
  char *outfile;                /* optional alignment output file           */
  int   outfmt;			/* code for output alignment format         */
  int   do_oneline;             /* TRUE to do one line/seq, no interleaving */
  FILE *ofp;                    /* handle on alignment output file          */
  char *withali;                /* name of additional alignment file to align */
  char *mapali;                 /* name of additional alignment file to map   */
  
  /*********************************************** 
   * Parse command line
   ***********************************************/
  
  format     = SQFILE_UNKNOWN;	  /* default: autodetect input format     */
  outfmt     = MSAFILE_STOCKHOLM; /* default: output in Stockholm format  */
  do_oneline = FALSE;		  /* default: interleaved format          */
  matchonly  = FALSE;
  outfile    = NULL;		  /* default: output alignment to stdout  */
  be_quiet   = FALSE;
  withali    = NULL;
  mapali     = NULL;

  while (Getopt(argc, argv, OPTIONS, NOPTIONS, usage,
                &optind, &optname, &optarg))  {
    if      (strcmp(optname, "-m")        == 0) matchonly  = TRUE;
    else if (strcmp(optname, "-o")        == 0) outfile    = optarg;
    else if (strcmp(optname, "-q")        == 0) be_quiet   = TRUE; 
    else if (strcmp(optname, "--mapali")  == 0) mapali     = optarg;
    else if (strcmp(optname, "--oneline") == 0) do_oneline = TRUE;
    else if (strcmp(optname, "--withali") == 0) withali    = optarg;
    else if (strcmp(optname, "--informat") == 0) 
      {
	format = String2SeqfileFormat(optarg);
	if (format == SQFILE_UNKNOWN) 
	  Die("unrecognized sequence file format \"%s\"", optarg);
      }
    else if (strcmp(optname, "--outformat") == 0) 
      {
	outfmt = String2SeqfileFormat(optarg);
	if (outfmt == MSAFILE_UNKNOWN) 
	  Die("unrecognized output alignment file format \"%s\"", optarg);
	if (! IsAlignmentFormat(outfmt))
	  Die("\"%s\" is not a multiple alignment format", optarg);
      }
    else if (strcmp(optname, "-h") == 0) 
      {
	HMMERBanner(stdout, banner);
	puts(usage);
	puts(experts);
	exit(EXIT_SUCCESS);
      }
  }
  if (argc - optind != 2)
    Die("Incorrect number of arguments.\n%s\n", usage);

  hmmfile = argv[optind++];
  seqfile = argv[optind++]; 

  /* Try to work around inability to autodetect from a pipe or .gz:
   * assume FASTA format
   */
  if (format == SQFILE_UNKNOWN &&
      (Strparse("^.*\\.gz$", seqfile, 0) || strcmp(seqfile, "-") == 0))
    format = SQFILE_FASTA;

 /*********************************************** 
  * Open HMM file (might be in HMMERDB or current directory).
  * Read a single HMM from it.
  * 
  * Currently hmmalign disallows the J state and
  * only allows one domain per sequence. To preserve
  * the S/W entry information, the J state is explicitly
  * disallowed, rather than calling a Plan7*Config() function.
  * this is a workaround in 2.1 for the 2.0.x "yo!" bug.
  ***********************************************/

  if ((hmmfp = HMMFileOpen(hmmfile, "HMMERDB")) == NULL)
    Die("Failed to open HMM file %s\n%s", hmmfile, usage);
  if (!HMMFileRead(hmmfp, &hmm)) 
    Die("Failed to read any HMMs from %s\n", hmmfile);
  HMMFileClose(hmmfp);
  if (hmm == NULL) 
    Die("HMM file %s corrupt or in incorrect format? Parse failed", hmmfile);
  hmm->xt[XTE][MOVE]  = 1.;	/* only 1 domain/sequence ("global" alignment) */
  hmm->xt[XTE][LOOP]  = 0.;
  hmm->xsc[XTE][MOVE] = 0;	/* unwarranted chumminess with log-odds score calculation */
  hmm->xsc[XTE][LOOP] = -INFTY;	
  
				/* do we have the map we might need? */
  if (mapali != NULL && ! (hmm->flags & PLAN7_MAP))
    Die("HMMER: HMM file %s has no map; you can't use --mapali.", hmmfile);

  /*********************************************** 
   * Open sequence file in current directory.
   * Read all seqs from it.
   ***********************************************/

  if (! ReadMultipleRseqs(seqfile, format, &rseq, &sqinfo, &nseq))
    Die("Failed to read any sequences from file %s", seqfile);

  /*********************************************** 
   * Show the banner
   ***********************************************/

  if (! be_quiet) 
    {
      HMMERBanner(stdout, banner);
      printf(   "HMM file:             %s\n", hmmfile);
      printf(   "Sequence file:        %s\n", seqfile);
      printf("- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -\n\n");
    }

  /*********************************************** 
   * Do the work
   ***********************************************/

  /* Allocations and initializations.
   */
  dsq = MallocOrDie(sizeof(unsigned char *)    * nseq);
  tr  = MallocOrDie(sizeof(struct p7trace_s *) * nseq);
  mx  = CreatePlan7Matrix(1, hmm->M, 25, 0);

  /* Align each sequence to the model, collect traces
   */
  for (i = 0; i < nseq; i++)
    {
      dsq[i] = DigitizeSequence(rseq[i], sqinfo[i].len);

      if (P7ViterbiSpaceOK(sqinfo[i].len, hmm->M, mx))
	(void) P7Viterbi(dsq[i], sqinfo[i].len, hmm, mx, &(tr[i]));
      else
	(void) P7SmallViterbi(dsq[i], sqinfo[i].len, hmm, mx, &(tr[i]));
    }
  FreePlan7Matrix(mx);

  /* Include an aligned alignment, if desired.
   */
  if (mapali != NULL)
    include_alignment(mapali, hmm, TRUE, &rseq, &dsq, &sqinfo, &tr, &nseq);
  if (withali != NULL) 
    include_alignment(withali, hmm, FALSE, &rseq, &dsq, &sqinfo, &tr, &nseq);

  /* Turn traces into a multiple alignment
   */ 
  wgt = MallocOrDie(sizeof(float) * nseq);
  FSet(wgt, nseq, 1.0);
  msa = P7Traces2Alignment(dsq, sqinfo, wgt, nseq, hmm->M, tr, matchonly);

  /*********************************************** 
   * Output the alignment
   ***********************************************/
  
  if (outfile != NULL && (ofp = fopen(outfile, "w")) != NULL)
    {
      MSAFileWrite(ofp, msa, outfmt, do_oneline);
      printf("Alignment saved in file %s\n", outfile);
      fclose(ofp);
    }
  else
    MSAFileWrite(stdout, msa, outfmt, do_oneline);


  /*********************************************** 
   * Cleanup and exit
   ***********************************************/
  
  for (i = 0; i < nseq; i++) 
    {
      P7FreeTrace(tr[i]);
      FreeSequence(rseq[i], &(sqinfo[i]));
      free(dsq[i]);
    }
  MSAFree(msa);
  FreePlan7(hmm);
  free(sqinfo);
  free(rseq);
  free(dsq);
  free(wgt);
  free(tr);

  SqdClean();
  return 0;
}


/* Function: include_alignment()
 * Date:     SRE, Sun Jul  5 15:25:13 1998 [St. Louis]
 *
 * Purpose:  Given the name of a multiple alignment file,
 *           align that alignment to the HMM, and add traces
 *           to an existing array of traces. If do_mapped
 *           is TRUE, we use the HMM's map file. If not,
 *           we use P7ViterbiAlignAlignment().
 *
 * Args:     seqfile  - name of alignment file
 *           hmm      - model to align to
 *           do_mapped- TRUE if we're to use the HMM's alignment map
 *           rsq      - RETURN: array of rseqs to add to
 *           dsq      - RETURN: array of dsq to add to
 *           sqinfo   - RETURN: array of SQINFO to add to
 *           tr       - RETURN: array of traces to add to
 *           nseq     - RETURN: number of seqs           
 *
 * Returns:  new, realloc'ed arrays for rsq, dsq, sqinfo, tr; nseq is
 *           increased to nseq+ainfo.nseq.
 */
static void
include_alignment(char *seqfile, struct plan7_s *hmm, int do_mapped,
		  char ***rsq, unsigned char ***dsq, SQINFO **sqinfo, 
		  struct p7trace_s ***tr, int *nseq)
{
  int format;			/* format of alignment file */
  MSA   *msa;			/* alignment to align to    */
  MSAFILE *afp;
  SQINFO  *newinfo;		/* sqinfo array from msa */
  unsigned char **newdsq;
  char **newrseq;
  int   idx;			/* counter over aseqs       */
  struct p7trace_s *master;     /* master trace             */
  struct p7trace_s **addtr;     /* individual traces for aseq */

  format = MSAFILE_UNKNOWN;	/* invoke Babelfish */
  if ((afp = MSAFileOpen(seqfile, format, NULL)) == NULL)
    Die("Alignment file %s could not be opened for reading", seqfile);
  if ((msa = MSAFileRead(afp)) == NULL)
    Die("Failed to read an alignment from %s\n", seqfile);
  MSAFileClose(afp);
  for (idx = 0; idx < msa->nseq; idx++)
    s2upper(msa->aseq[idx]);
  newinfo = MSAToSqinfo(msa);

				/* Verify checksums before mapping */
  if (do_mapped && GCGMultchecksum(msa->aseq, msa->nseq) != hmm->checksum)
    Die("The checksums for alignment file %s and the HMM alignment map don't match.", 
	seqfile);
				/* Get a master trace */
  if (do_mapped) master = MasterTraceFromMap(hmm->map, hmm->M, msa->alen);
  else           master = P7ViterbiAlignAlignment(msa, hmm);

				/* convert to individual traces */
  ImposeMasterTrace(msa->aseq, msa->nseq, master, &addtr);
				/* add those traces to existing ones */
  *tr = MergeTraceArrays(*tr, *nseq, addtr, msa->nseq);
  
				/* additional bookkeeping: add to dsq, sqinfo */
  *rsq = ReallocOrDie((*rsq), sizeof(char *) * (*nseq + msa->nseq));
  DealignAseqs(msa->aseq, msa->nseq, &newrseq);
  for (idx = *nseq; idx < *nseq + msa->nseq; idx++)
    (*rsq)[idx] = newrseq[idx - (*nseq)];
  free(newrseq);

  *dsq = ReallocOrDie((*dsq), sizeof(unsigned char *) * (*nseq + msa->nseq));
  DigitizeAlignment(msa, &newdsq);
  for (idx = *nseq; idx < *nseq + msa->nseq; idx++)
    (*dsq)[idx] = newdsq[idx - (*nseq)];
  free(newdsq);
			/* unnecessarily complex, but I can't be bothered... */
  *sqinfo = ReallocOrDie((*sqinfo), sizeof(SQINFO) * (*nseq + msa->nseq));
  for (idx = *nseq; idx < *nseq + msa->nseq; idx++)
    SeqinfoCopy(&((*sqinfo)[idx]), &(newinfo[idx - (*nseq)]));
  free(newinfo);
  
  *nseq = *nseq + msa->nseq;

				/* Cleanup */
  P7FreeTrace(master);
  MSAFree(msa);
				/* Return */
  return;
}


/* Function: MergeTraceArrays()
 * Date:     SRE, Sun Jul  5 15:09:10 1998 [St. Louis]
 *
 * Purpose:  Combine two arrays of traces into a single array.
 *           Used in hmmalign to merge traces from a fixed alignment
 *           with traces from individual unaligned seqs.
 * 
 *           t1 traces always precede t2 traces in the resulting array.
 *
 * Args:     t1 - first set of traces
 *           n1 - number of traces in t1
 *           t2 - second set of traces
 *           n2 - number of traces in t2
 *
 * Returns:  pointer to new array of traces.
 *           Both t1 and t2 are free'd here! Do not reuse.
 */
struct p7trace_s **
MergeTraceArrays(struct p7trace_s **t1, int n1, struct p7trace_s **t2, int n2)
{
  struct p7trace_s **tr;
  int i;			/* index in traces */

  tr = MallocOrDie(sizeof(struct p7trace_s *) * (n1+n2));
  for (i = 0; i < n1; i++) tr[i]    = t1[i];
  for (i = 0; i < n2; i++) tr[n1+i] = t2[i];
  free(t1);
  free(t2);
  return tr;
}

/************************************************************
 * @LICENSE@
 ************************************************************/

