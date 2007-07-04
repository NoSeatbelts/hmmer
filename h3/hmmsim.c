/* main() for scoring profile HMMs against simulated sequences
 * 
 * Example:
 *  ./hmmbuild Pfam /misc/data0/databases/Pfam/Pfam-A.seed                
 *  qsub -N testrun -j y -R y -b y -cwd -V -pe lam-mpi-tight 32 'mpirun C ./mpi-hmmsim -N 10000 ./Pfam > foo.out'
 * 
 * SRE, Fri Apr 20 14:56:26 2007 [Janelia]
 * SVN $Id$
 */
#include "p7_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef HAVE_MPI
#include "mpi.h"
#endif 

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_exponential.h"
#include "esl_dmatrix.h"
#include "esl_getopts.h"
#include "esl_gumbel.h"
#include "esl_histogram.h"
#include "esl_mpi.h"
#include "esl_ratematrix.h"
#include "esl_stopwatch.h"
#include "esl_vectorops.h"

#include "hmmer.h"
#include "impl_fp.h"

#define ALGORITHMS "--fwd,--viterbi,--island,--hybrid" /* Exclusive choice for scoring algorithms */
#define STYLES     "--fs,--sw,--ls,--s"	               /* Exclusive choice for alignment mode     */

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range     toggles   reqs   incomp  help   docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, NULL, "show brief help on version and usage",            1 },
  { "-L",        eslARG_INT,    "100", NULL, "n>0",     NULL,  NULL, NULL, "length of random target seqs",                    1 },
  { "-N",        eslARG_INT,   "1000", NULL, "n>0",     NULL,  NULL, NULL, "number of random target seqs",                    1 },
#ifdef HAVE_MPI
  { "--mpi",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, NULL, "run as an MPI parallel program",                  1 },
#endif
  { "-o",        eslARG_OUTFILE, NULL, NULL, NULL,      NULL,  NULL, NULL, "direct output to file <f>, not stdout",           2 },
  { "--pfile",   eslARG_OUTFILE, NULL, NULL, NULL,      NULL,  NULL, NULL, "output P(S>x) plots to <f> in xy format",         2 },
  { "--xfile",   eslARG_OUTFILE, NULL, NULL, NULL,      NULL,  NULL, NULL, "output bitscores as binary double vector to <f>", 2 },

  { "--fs",      eslARG_NONE,"default",NULL, NULL,    STYLES,  NULL, NULL, "multihit local alignment",                    3 },
  { "--sw",      eslARG_NONE,   FALSE, NULL, NULL,    STYLES,  NULL, NULL, "unihit local alignment",                      3 },
  { "--ls",      eslARG_NONE,   FALSE, NULL, NULL,    STYLES,  NULL, NULL, "multihit glocal alignment",                   3 },
  { "--s",       eslARG_NONE,   FALSE, NULL, NULL,    STYLES,  NULL, NULL, "unihit glocal alignment",                     3 },

  { "--viterbi", eslARG_NONE,"default",NULL, NULL, ALGORITHMS, NULL, NULL, "Score seqs with the Viterbi algorithm",       4 },
  { "--fwd",     eslARG_NONE,   FALSE, NULL, NULL, ALGORITHMS, NULL, NULL, "Score seqs with the Forward algorithm",       4 },
  { "--hybrid",  eslARG_NONE,   FALSE, NULL, NULL, ALGORITHMS, NULL, NULL, "Score seqs with the Hybrid algorithm",        4 },

  { "--tmin",    eslARG_REAL,  "0.02", NULL, NULL,      NULL,  NULL, NULL, "Set lower bound tail mass for fwd,island",    5 },
  { "--tmax",    eslARG_REAL,  "0.02", NULL, NULL,      NULL,  NULL, NULL, "Set lower bound tail mass for fwd,island",    5 },
  { "--tstep",   eslARG_REAL,  "0.02", NULL, NULL,      NULL,  NULL, NULL, "Set additive step size for tmin...tmax range",5 },
/* Debugging options */
  { "--oprofile",eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, NULL, "use experimental fp implementation",                6 },  
  { "--bgflat",  eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, NULL, "set uniform background frequencies",                6 },  
  { "--bgcomp",  eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, NULL, "set bg frequencies to model's average composition", 6 },
  { "--stall",   eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, NULL, "arrest after start: for debugging MPI under gdb",   6 },  
  { "--seed",    eslARG_INT,     NULL, NULL, NULL,      NULL,  NULL, NULL, "set random number seed to <n>",                     6 },  
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* struct cfg_s : "Global" application configuration shared by all threads/processes.
 * 
 * This structure is passed to routines within main.c, as a means of semi-encapsulation
 * of shared data amongst different parallel processes (threads or MPI processes).
 */
struct cfg_s {
  /* Shared configuration in masters & workers */
  char           *hmmfile;	/* name of input HMM file  */ 
  ESL_RANDOMNESS *r;		/* randomness source       */
  ESL_ALPHABET   *abc;		/* alphabet type, eslAMINO */
  P7_BG          *bg;		/* background model        */
  int             my_rank;	/* 0 in masters, >0 in workers     */
  int             nproc;	/* 1 in serial mode, >1 in MPI     */
  int             do_mpi;	/* TRUE if we're --mpi             */
  int             do_stall;	/* TRUE to stall for MPI debugging */
  int             N;		/* number of simulated seqs per HMM */
  int             L;		/* length of simulated seqs */

  /* Masters only (i/o streams) */
  P7_HMMFILE     *hfp;		/* open input HMM file stream */
  FILE           *ofp;		/* output file for results (default is stdout) */
  FILE           *survfp;	/* optional output for survival plots */
  FILE           *xfp;		/* optional output for binary score vectors */
};

static char usage[]  = "[-options] <hmmfile>";
static char banner[] = "collect profile HMM score distributions on random sequences";


static int  init_master_cfg(ESL_GETOPTS *go, struct cfg_s *cfg, char *errbuf);

static void serial_master  (ESL_GETOPTS *go, struct cfg_s *cfg);
#ifdef HAVE_MPI
static void mpi_master     (ESL_GETOPTS *go, struct cfg_s *cfg);
static void mpi_worker     (ESL_GETOPTS *go, struct cfg_s *cfg);
#endif 
static int process_workunit(ESL_GETOPTS *go, struct cfg_s *cfg, char *errbuf, P7_HMM *hmm, double *scores);
static int output_result   (ESL_GETOPTS *go, struct cfg_s *cfg, char *errbuf, P7_HMM *hmm, double *scores);

static int minimum_mpi_working_buffer(int N, int *ret_wn);

int
main(int argc, char **argv)
{
  ESL_GETOPTS     *go	   = NULL;      /* command line processing                   */
  ESL_STOPWATCH   *w       = esl_stopwatch_Create();
  struct cfg_s     cfg;

  /* Process command line options.
   */
  go = esl_getopts_Create(options);
  if (esl_opt_ProcessCmdline(go, argc, argv) != eslOK || 
      esl_opt_VerifyConfig(go)               != eslOK)
    {
      printf("Failed to parse command line: %s\n", go->errbuf);
      esl_usage(stdout, argv[0], usage);
      printf("\nTo see more help on available options, do %s -h\n\n", argv[0]);
      exit(1);
    }
  if (esl_opt_GetBoolean(go, "-h") == TRUE) 
    {
      p7_banner(stdout, argv[0], banner);
      esl_usage(stdout, argv[0], usage);
      puts("\nwhere general options are:");
      esl_opt_DisplayHelp(stdout, go, 1, 2, 80); /* 1=docgroup, 2 = indentation; 80=textwidth*/
      puts("\noutput options (only in serial mode, for single HMM input):");
      esl_opt_DisplayHelp(stdout, go, 2, 2, 80); 
      puts("\nalternative alignment styles :");
      esl_opt_DisplayHelp(stdout, go, 3, 2, 80);
      puts("\nalternative scoring algorithms :");
      esl_opt_DisplayHelp(stdout, go, 4, 2, 80);
      puts("\ncontrolling range of fitted tail masses :");
      esl_opt_DisplayHelp(stdout, go, 5, 2, 80);
      puts("\ndebugging :");
      esl_opt_DisplayHelp(stdout, go, 6, 2, 80);
      exit(0);
    }
  if (esl_opt_ArgNumber(go) != 1) 
    {
      puts("Incorrect number of command line arguments.");
      esl_usage(stdout, argv[0], usage);
      printf("\nTo see more help on available options, do %s -h\n\n", argv[0]);
      exit(1);
    }

  /* Initialize configuration shared across all kinds of masters
   * and workers in this .c file.
   */
  cfg.hmmfile  = esl_opt_GetArg(go, 1);
  if (! esl_opt_IsDefault(go, "--seed"))  cfg.r = esl_randomness_Create(esl_opt_GetInteger(go, "--seed"));
  else                                    cfg.r = esl_randomness_CreateTimeseeded();
  cfg.abc      = esl_alphabet_Create(eslAMINO);

  if (esl_opt_GetBoolean(go, "--bgflat")) cfg.bg = p7_bg_CreateUniform(cfg.abc);
  else                                    cfg.bg = p7_bg_Create(cfg.abc);

  cfg.my_rank  = 0;		/* MPI init will change this soon, if --mpi was set */
  cfg.nproc    = 0;		/* MPI init will change this soon, if --mpi was set */
  cfg.do_mpi   = FALSE;		/* --mpi will change this soon (below) if necessary */
  cfg.do_stall = esl_opt_GetBoolean(go, "--stall");
  cfg.N        = esl_opt_GetInteger(go, "-N");
  cfg.L        = esl_opt_GetInteger(go, "-L");
  cfg.hfp      = NULL;
  cfg.ofp      = NULL;
  cfg.survfp   = NULL;
  cfg.xfp      = NULL;

  p7_bg_SetLength(cfg.bg, esl_opt_GetInteger(go, "-L"));

  /* This is our stall point, if we need to wait until we get a
   * debugger attached to this process for debugging (especially
   * useful for MPI):
   */
  while (cfg.do_stall); 

  /* Start timing. */
  esl_stopwatch_Start(w);

  /* Initialize MPI, figure out who we are, and whether we're running
   * this show (proc 0) or working in it (procs >0)
   */
#ifdef HAVE_MPI
  if (esl_opt_GetBoolean(go, "--mpi")) 
    {
      cfg.do_mpi = TRUE;
      MPI_Init(&argc, &argv);
      MPI_Comm_rank(MPI_COMM_WORLD, &(cfg.my_rank));
      MPI_Comm_size(MPI_COMM_WORLD, &(cfg.nproc));
      if (cfg.my_rank == 0 && cfg.nproc < 2) p7_Fail("Need at least 2 MPI processes to run --mpi mode.");

      if (cfg.my_rank > 0)   mpi_worker(go, &cfg);
      else                   mpi_master(go, &cfg);

      esl_stopwatch_Stop(w);
      esl_stopwatch_MPIReduce(w, 0, MPI_COMM_WORLD);
      MPI_Finalize();		/* both workers and masters reach this line */
    }
  else
#endif /*HAVE_MPI*/
    {				/*  we're the serial master */
      serial_master(go, &cfg);
      esl_stopwatch_Stop(w);
    }      

  if (cfg.my_rank == 0) esl_stopwatch_Display(stdout, w, "# CPU time: ");

  if (cfg.my_rank == 0) {
    if (cfg.hfp    != NULL)             p7_hmmfile_Close(cfg.hfp);
    if (! esl_opt_IsDefault(go, "-o"))  fclose(cfg.ofp); 
    if (cfg.survfp != NULL)             fclose(cfg.survfp);
    if (cfg.xfp    != NULL)             fclose(cfg.xfp);
  }
  if (cfg.bg  != NULL) p7_bg_Destroy(cfg.bg);
  if (cfg.abc != NULL) esl_alphabet_Destroy(cfg.abc);
  if (cfg.r   != NULL) esl_randomness_Destroy(cfg.r);
  if (go      != NULL) esl_getopts_Destroy(go);
  return eslOK;
}

/* init_master_cfg()
 * Called by masters, mpi or serial.
 * Already set:
 *    cfg->hmmfile - command line arg 
 * Sets:
 *    cfg->hfp     - open HMM stream
 *    cfg->ofp     - open output steam
 *    cfg->survfp  - open xmgrace survival plot file 
 *    cfg->xfp     - open binary score file
 *
 * Error handling relies on the result pointers being initialized to
 * NULL by the caller.
 *                   
 * Errors in the MPI master here are considered to be "recoverable",
 * in the sense that we'll try to delay output of the error message
 * until we've cleanly shut down the worker processes. Therefore
 * errors return (code, errmsg) by the ESL_FAIL mech.
 */
static int
init_master_cfg(ESL_GETOPTS *go, struct cfg_s *cfg, char *errbuf)
{
  char *filename;
  int   status;

  status = p7_hmmfile_Open(cfg->hmmfile, NULL, &(cfg->hfp));
  if (status == eslENOTFOUND) ESL_FAIL(eslFAIL, errbuf, "Failed to open hmm file %s for reading.\n",  cfg->hmmfile);
  else if (status != eslOK)   ESL_FAIL(eslFAIL, errbuf, "Unexpected error in opening hmm file %s.\n", cfg->hmmfile);

  filename = esl_opt_GetString(go, "-o");
  if (filename != NULL) 
    {
      if ((cfg->ofp = fopen(filename, "w")) == NULL) 
	ESL_FAIL(eslFAIL, errbuf, "Failed to open -o output file %s\n", filename);
    } 
  else cfg->ofp = stdout;

  filename = esl_opt_GetString(go, "--pfile");
  if (filename != NULL) 
    {
      if ((cfg->survfp = fopen(filename, "w")) == NULL) 
	ESL_FAIL(eslFAIL, errbuf, "Failed to open --pfile output file %s\n", filename);
    }

  filename = esl_opt_GetString(go, "--xfile");
  if (filename != NULL) 
    {
      if ((cfg->xfp = fopen(filename, "w")) == NULL) 
	ESL_FAIL(eslFAIL, errbuf, "Failed to open --xfile output file %s\n", filename);
    }

  return eslOK;
}




static void
serial_master(ESL_GETOPTS *go, struct cfg_s *cfg)
{
  P7_HMM     *hmm = NULL;     
  double     *xv  = NULL;
  char        errbuf[eslERRBUFSIZE];
  int         status;

  if ((status = init_master_cfg(go, cfg, errbuf)) != eslOK) p7_Fail(errbuf);
  if ((xv = malloc(sizeof(double) * cfg->N)) == NULL)       p7_Fail("allocation failed");

  while ((status = p7_hmmfile_Read(cfg->hfp, &(cfg->abc), &hmm)) != eslEOF) 
    {
      if      (status == eslEOD)       p7_Fail("read failed, HMM file %s may be truncated?", cfg->hmmfile);
      else if (status == eslEFORMAT)   p7_Fail("bad file format in HMM file %s",             cfg->hmmfile);
      else if (status == eslEINCOMPAT) p7_Fail("HMM file %s contains different alphabets",   cfg->hmmfile);
      else if (status != eslOK)        p7_Fail("Unexpected error in reading HMMs from %s",   cfg->hmmfile);

      if (process_workunit(go, cfg, errbuf, hmm, xv) != eslOK) p7_Fail(errbuf);
      if (output_result   (go, cfg, errbuf, hmm, xv) != eslOK) p7_Fail(errbuf);

      p7_hmm_Destroy(hmm);      
    }
  free(xv);
}


#ifdef HAVE_MPI
/* mpi_master()
 * The MPI version of hmmsim.
 * Follows standard pattern for a master/worker load-balanced MPI program (J1/78-79).
 * 
 * A master can only return if it's successful. 
 * Errors in an MPI master come in two classes: recoverable and nonrecoverable.
 * 
 * Recoverable errors include all worker-side errors, and any
 * master-side error that do not affect MPI communication. Error
 * messages from recoverable messages are delayed until we've cleanly
 * shut down the workers.
 * 
 * Unrecoverable errors are master-side errors that may affect MPI
 * communication, meaning we cannot count on being able to reach the
 * workers and shut them down. Unrecoverable errors result in immediate
 * p7_Fail()'s, which will cause MPI to shut down the worker processes
 * uncleanly.
 */
static void
mpi_master(ESL_GETOPTS *go, struct cfg_s *cfg)
{
  int              xstatus       = eslOK; /* changes in the event of a recoverable error */
  P7_HMM          *hmm           = NULL;  /* query HMM                                 */
  P7_HMM         **hmmlist       = NULL;  /* queue of HMMs being worked on, 1..nproc-1 */
  char            *wbuf          = NULL;  /* working buffer for sending packed profiles to workers. */
  int              wn            = 0;
  double          *xv            = NULL;
  int              have_work     = TRUE;
  int              nproc_working = 0;
  int              wi;
  int              pos;
  char             errbuf[eslERRBUFSIZE];
  int              status;
  MPI_Status       mpistatus;

  /* Master initialization. */
  if (init_master_cfg(go, cfg, errbuf)        != eslOK)   p7_Fail(errbuf);
  if (minimum_mpi_working_buffer(cfg->N, &wn) != eslOK)   p7_Fail("mpi pack sizes must have failed");
  ESL_ALLOC(wbuf,    sizeof(char)     * wn);
  ESL_ALLOC(xv,      sizeof(double)   * cfg->N);
  ESL_ALLOC(hmmlist, sizeof(P7_HMM *) * cfg->nproc);
  for (wi = 0; wi < cfg->nproc; wi++) hmmlist[wi] = NULL;

  /* Standard design pattern for data parallelization in a master/worker model. (J1/78-79).  */
  wi = 1;
  while (have_work || nproc_working)
    {
      /* Get next work unit: one HMM, <hmm> */
      if (have_work) 
	{
	  if ((status = p7_hmmfile_Read(cfg->hfp, &(cfg->abc), &hmm)) != eslOK) 
	    {
	      have_work = FALSE;
	      if      (status == eslEOD)       { xstatus = status; sprintf(errbuf, "read failed, HMM file %s may be truncated?", cfg->hmmfile); }
	      else if (status == eslEFORMAT)   { xstatus = status; sprintf(errbuf, "bad file format in HMM file %s",             cfg->hmmfile); }
	      else if (status == eslEINCOMPAT) { xstatus = status; sprintf(errbuf, "HMM file %s contains different alphabets",   cfg->hmmfile); }
	      else if (status != eslEOF)       { xstatus = status; sprintf(errbuf, "Unexpected error in reading HMMs from %s",   cfg->hmmfile); }
	    }
	}

      /* If we have work but no free workers, or we have no work but workers
       * are still working, then wait for a result to return from any worker.
       */
      if ( (have_work && nproc_working == cfg->nproc-1) || (! have_work && nproc_working > 0))
	{
	  if (MPI_Recv(wbuf, wn, MPI_PACKED, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &mpistatus) != 0) p7_Fail("mpi recv failed");
	  wi = mpistatus.MPI_SOURCE;
	  
	  /* Check the xstatus before printing results.
           * If we're in a recoverable error state, we're only clearing worker results, prior to a clean failure
	   */
	  if (xstatus == eslOK)	
	    {
	      pos = 0;
	      if (MPI_Unpack(wbuf, wn, &pos, &xstatus, 1, MPI_INT, MPI_COMM_WORLD)     != 0)     p7_Fail("mpi unpack failed");
	      if (xstatus == eslOK) /* worker reported success. Get the results. */
		{
		  if (MPI_Unpack(wbuf, wn, &pos, xv, cfg->N, MPI_DOUBLE, MPI_COMM_WORLD) != 0)     p7_Fail("score vector unpack failed");
		  if ((status = output_result(go, cfg, errbuf, hmmlist[wi], xv))         != eslOK) xstatus = status;
		}
	      else	/* worker reported a user error. Get the errbuf. */
		{
		  if (MPI_Unpack(wbuf, wn, &pos, errbuf, eslERRBUFSIZE, MPI_CHAR, MPI_COMM_WORLD) != 0) p7_Fail("mpi unpack of errbuf failed");
		  have_work = FALSE;
		  p7_hmm_Destroy(hmm);
		}
	    }
	  p7_hmm_Destroy(hmmlist[wi]);
	  hmmlist[wi] = NULL;
	  nproc_working--;
	}
	
      /* If we have work, assign it to a free worker; else, terminate the free worker. */
      if (have_work) 
	{
	  p7_hmm_MPISend(hmm, wi, 0, MPI_COMM_WORLD, &wbuf, &wn);
	  hmmlist[wi] = hmm;
	  wi++;
	  nproc_working++;
	}
    }

  /* Tell all the workers (1..nproc-1) to shut down by sending them a NULL workunit. */
  for (wi = 1; wi < cfg->nproc; wi++)
    if (p7_hmm_MPISend(NULL, wi, 0, MPI_COMM_WORLD, &wbuf, &wn) != eslOK) p7_Fail("MPI HMM send failed");	


  free(hmmlist);
  free(wbuf);
  free(xv);
  if (xstatus != eslOK) p7_Fail(errbuf);
  else                  return;

 ERROR:
  if (hmmlist != NULL) free(hmmlist);
  if (wbuf    != NULL) free(wbuf);
  if (xv      != NULL) free(xv);
  p7_Fail("Fatal error in mpi_master");
}


/* mpi_worker()
 * The main control for an MPI worker process.
 */
static void
mpi_worker(ESL_GETOPTS *go, struct cfg_s *cfg)
{
  int             xstatus = eslOK;
  int             status;
  P7_HMM         *hmm     = NULL;
  char           *wbuf    = NULL;
  double         *xv      = NULL;
  int             wn      = 0;
  char            errbuf[eslERRBUFSIZE];
  int             pos;
 
  /* Worker initializes */
  if ((status = minimum_mpi_working_buffer(cfg->N, &wn)) != eslOK) xstatus = status;
  ESL_ALLOC(wbuf, wn * sizeof(char));
  ESL_ALLOC(xv,   cfg->N * sizeof(double));

  /* Main worker loop */
  while (p7_hmm_MPIRecv(0, 0, MPI_COMM_WORLD, &wbuf, &wn, &(cfg->abc), &hmm) == eslOK) 
    {
      if ((status = process_workunit(go, cfg, errbuf, hmm, xv)) != eslOK) goto CLEANERROR;

      pos = 0;
      MPI_Pack(&status, 1,      MPI_INT,    wbuf, wn, &pos, MPI_COMM_WORLD);
      MPI_Pack(xv,      cfg->N, MPI_DOUBLE, wbuf, wn, &pos, MPI_COMM_WORLD);
      MPI_Send(wbuf, pos, MPI_PACKED, 0, 0, MPI_COMM_WORLD);

      p7_hmm_Destroy(hmm);
    }

  if (wbuf != NULL) free(wbuf);
  return;

 CLEANERROR:
  pos = 0;
  MPI_Pack(&status, 1,                MPI_INT,  wbuf, wn, &pos, MPI_COMM_WORLD);
  MPI_Pack(errbuf,  eslERRBUFSIZE,    MPI_CHAR, wbuf, wn, &pos, MPI_COMM_WORLD);
  MPI_Send(wbuf, pos, MPI_PACKED, 0, 0, MPI_COMM_WORLD);
  if (wbuf != NULL) free(wbuf);
  if (hmm  != NULL) p7_hmm_Destroy(hmm);
  return;

 ERROR:
  p7_Fail("Allocation error in mpi_worker");
}
#endif /*HAVE_MPI*/


/* A work unit consists of one HMM, <hmm>.
 * The result is the <scores> array, which contains an array of N scores;
 * caller provides this memory.
 * How those scores are generated is controlled by the application configuration in <cfg>.
 */
static int
process_workunit(ESL_GETOPTS *go, struct cfg_s *cfg, char *errbuf, P7_HMM *hmm, double *scores)
{
  int             L   = esl_opt_GetInteger(go, "-L");
  P7_PROFILE     *gm  = NULL;
  P7_GMX         *gx  = NULL;
  P7_OPROFILE    *om  = NULL;
  P7_OMX         *ox  = NULL;
  ESL_DSQ        *dsq = NULL;
  int             i;
  int             status;

  /* Optionally set a custom background, determined by model composition;
   * an experimental hack. 
   */
  if (esl_opt_GetBoolean(go, "--bgcomp")) 
    {
      float *p = NULL;
      float  KL;

      p7_hmm_CompositionKLDist(hmm, cfg->bg, &KL, &p);
      esl_vec_FCopy(p, cfg->abc->K, cfg->bg->f);
    }


  /* Prepare the score profile. */
  if (esl_opt_GetBoolean(go, "--oprofile")) 
    {
      om = p7_oprofile_Create(hmm->M, cfg->abc);
      if      (esl_opt_GetBoolean(go, "--fs"))  p7_oprofile_Config(hmm, cfg->bg, om, p7_LOCAL);
      else if (esl_opt_GetBoolean(go, "--sw"))  p7_oprofile_Config(hmm, cfg->bg, om, p7_UNILOCAL);
      else if (esl_opt_GetBoolean(go, "--ls"))  p7_oprofile_Config(hmm, cfg->bg, om, p7_GLOCAL);
      else if (esl_opt_GetBoolean(go, "--s"))   p7_oprofile_Config(hmm, cfg->bg, om, p7_UNIGLOCAL);
      p7_oprofile_ReconfigLength(om, L);
      ox = p7_omx_Create(om->M, L);
    }
  else 
    {
      gm = p7_profile_Create(hmm->M, cfg->abc);
      if      (esl_opt_GetBoolean(go, "--fs"))  p7_ProfileConfig(hmm, cfg->bg, gm, p7_LOCAL);
      else if (esl_opt_GetBoolean(go, "--sw"))  p7_ProfileConfig(hmm, cfg->bg, gm, p7_UNILOCAL);
      else if (esl_opt_GetBoolean(go, "--ls"))  p7_ProfileConfig(hmm, cfg->bg, gm, p7_GLOCAL);
      else if (esl_opt_GetBoolean(go, "--s"))   p7_ProfileConfig(hmm, cfg->bg, gm, p7_UNIGLOCAL);
      p7_ReconfigLength(gm, L);
      gx = p7_gmx_Create(gm->M, L);
    }

  ESL_ALLOC(dsq, sizeof(ESL_DSQ) * (L+2));
  
  /* Collect scores from N random sequences of length L  */
  for (i = 0; i < cfg->N; i++)
    {
      esl_rnd_xfIID(cfg->r, cfg->bg->f, cfg->abc->K, L, dsq);

      if (esl_opt_GetBoolean(go, "--oprofile")) 
	{
	  float sc;

	  if (esl_opt_GetBoolean(go, "--viterbi"))
	    {
	      p7_Viterbi(dsq, L, om, ox, &sc);
	      scores[i] = sc - ((float) L * log(cfg->bg->p1) + log(1.-cfg->bg->p1)); /* equiv to subtracting p7_bg_NullOne, but in fp */
	      scores[i] /= eslCONST_LOG2;
	    }
	}
      else 			/* normal profiles */
	{
	  int  sc;
	  int  nullsc;

	  if      (esl_opt_GetBoolean(go, "--viterbi"))   p7_GViterbi(dsq, L, gm, gx, &sc);
	  else if (esl_opt_GetBoolean(go, "--fwd")) 	  p7_GForward(dsq, L, gm, gx, &sc);
	  else if (esl_opt_GetBoolean(go, "--hybrid")) 	  p7_GHybrid (dsq, L, gm, gx, NULL, &sc);

	  p7_bg_NullOne(cfg->bg, dsq, L, &nullsc);
	  scores[i] = p7_SILO2Bitscore(sc - nullsc);
	}
    }

  status = eslOK;
 ERROR:
  free(dsq);
  p7_profile_Destroy(gm);
  p7_oprofile_Destroy(om);
  p7_gmx_Destroy(gx);
  p7_omx_Destroy(ox);
  if (status == eslEMEM) sprintf(errbuf, "allocation failure");
  return status;
}


static int 
output_result(ESL_GETOPTS *go, struct cfg_s *cfg, char *errbuf, P7_HMM *hmm, double *scores)
{
  ESL_HISTOGRAM *h = NULL;
  int            i;
  double         tailp;
  double         x10;
  double         mu, lambda, E10;
  double         mufix, E10fix;
  int            status;

  /* Count the scores into a histogram object.  */
  if ((h = esl_histogram_CreateFull(-50., 50., 0.2)) == NULL) ESL_XFAIL(eslEMEM, errbuf, "allocation failed");
  for (i = 0; i < cfg->N; i++) esl_histogram_Add(h, scores[i]);

  /* For viterbi and hybrid, fit data to a Gumbel, either with known lambda or estimated lambda. */
  if (esl_opt_GetBoolean(go, "--viterbi") || esl_opt_GetBoolean(go, "--hybrid"))
    {
      esl_histogram_GetRank(h, 10, &x10);

      tailp  = 1.0;
      esl_gumbel_FitComplete(scores, cfg->N, &mu, &lambda);
      E10    = cfg->N * esl_gumbel_surv(x10, mu, lambda); 
      esl_gumbel_FitCompleteLoc(scores, cfg->N, 0.693147, &mufix);
      E10fix = cfg->N * esl_gumbel_surv(x10, mufix, 0.693147); 
      
      fprintf(cfg->ofp, "%-20s  %8.4f %8.4f %8.4f %8.4f %8.4f %8.4f\n", hmm->name, tailp, mu, lambda, E10, mufix, E10fix);

      if (cfg->survfp != NULL) {
	esl_histogram_PlotSurvival(cfg->survfp, h);
	esl_gumbel_Plot(cfg->survfp, mu,    lambda,   esl_gumbel_surv, h->xmin - 5., h->xmax + 5., 0.1);
	esl_gumbel_Plot(cfg->survfp, mufix, 0.693147, esl_gumbel_surv, h->xmin - 5., h->xmax + 5., 0.1);
      }
      
      if (cfg->xfp) 
	fwrite(scores, sizeof(double), cfg->N, cfg->xfp);
    }

  /* For Forward, fit tail to exponential tails, for a range of tail mass choices. */
  else if (esl_opt_GetBoolean(go, "--fwd"))
    {
      double  tmin  = esl_opt_GetReal(go, "--tmin");
      double  tmax  = esl_opt_GetReal(go, "--tmax");
      double  tstep = esl_opt_GetReal(go, "--tstep");
      double *xv;
      int     n;

      esl_histogram_GetRank(h, 10, &x10);

      for (tailp = tmin; tailp <= tmax+1e-7; tailp += tstep)
	{
	  esl_histogram_GetTailByMass(h, tailp, &xv, &n, NULL);

	  esl_exp_FitComplete(xv, n, &mu, &lambda);
	  E10    = cfg->N * tailp * esl_exp_surv(x10, mu, lambda);
	  mufix  = mu;
	  E10fix = cfg->N * tailp * esl_exp_surv(x10, mu, 0.693147);

	  printf("%-20s  %8.4f %8.4f %8.4f %8.4f %8.4f %8.4f\n", hmm->name, tailp, mu, lambda, E10, mufix, E10fix);

	}

      if (cfg->survfp) 
	{
	  esl_histogram_PlotSurvival(cfg->survfp, h);
	  esl_exp_Plot(cfg->survfp, mu,    lambda,     esl_exp_surv, mu, h->xmax + 5., 0.1);
	  esl_exp_Plot(cfg->survfp, mu,    0.693147,   esl_exp_surv, mu, h->xmax + 5., 0.1);
	}

      if (cfg->xfp) 
	fwrite(xv, sizeof(double), cfg->N, cfg->xfp);
    }


  /* fallthrough: both normal, error cases execute same cleanup code */
  status = eslOK;
 ERROR:
  esl_histogram_Destroy(h);
  return status;
}


/* the pack send/recv buffer must be big enough to hold either an error message or a result vector.
 * it may even grow larger than that, to hold largest HMM we send.
 */
static int
minimum_mpi_working_buffer(int N, int *ret_wn)
{
  int ncode, nerr, nresult;

  *ret_wn = 0;
  if (MPI_Pack_size(1,             MPI_INT,    MPI_COMM_WORLD, &ncode)    != 0) return eslESYS;
  if (MPI_Pack_size(N,             MPI_DOUBLE, MPI_COMM_WORLD, &nresult)  != 0) return eslESYS;
  if (MPI_Pack_size(eslERRBUFSIZE, MPI_CHAR,   MPI_COMM_WORLD, &nerr)     != 0) return eslESYS;

  *ret_wn = ncode + ESL_MAX(nresult, nerr);
  return eslOK;
}