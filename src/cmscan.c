/* cmscan: search sequence(s) against a covariance model database
 * 
 * EPN, Tue Jun 28 04:33:27 2011
 * SRE, Mon Oct 20 08:28:05 2008 [Janelia] (hmmscan.c)
 * SVN $Id$
 */
#include "esl_config.h"
#include "p7_config.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_getopts.h"
#include "esl_sq.h"
#include "esl_sqio.h"
#include "esl_stopwatch.h"
#include "esl_vectorops.h"

#ifdef HAVE_MPI
#include "mpi.h"
#include "esl_mpi.h"
#endif /*HAVE_MPI*/

#ifdef HMMER_THREADS
#include <unistd.h>
#include "esl_threads.h"
#include "esl_workqueue.h"
#endif /*HMMER_THREADS*/

#include "hmmer.h"
#include "funcs.h"		/* external functions                   */
#include "structs.h"		/* data structures, macros, #define's   */

typedef struct {
#ifdef HMMER_THREADS
  ESL_WORK_QUEUE   *queue;
#endif /*HMMER_THREADS*/
  ESL_SQ           *qsq;
  P7_BG            *bg;	             /* null model                                    */
  CM_PIPELINE      *pli;             /* work pipeline                                 */
  CM_TOPHITS       *th;              /* top hit results                               */
  float            *p7_evparam;      /* E-value parameters for p7 filter              */
  int               in_rc;           /* TRUE if qsq is currently revcomp'ed           */
} WORKER_INFO;


#define REPOPTS     "-E,-T,--cut_ga,--cut_nc,--cut_tc"
#define INCOPTS     "--incE,--incT,--cut_ga,--cut_nc,--cut_tc"
#define THRESHOPTS  "-E,-T,--incE,--incT,--cut_ga,--cut_nc,--cut_tc"
#define FMODEOPTS   "--FZ,--rfam,--mid,--nohmm,--max"
#define TIMINGOPTS  "--time-F1,--time-F2,--time-F3,--time-F4,--time-F5,--time-F6"

/* large sets of options are InCompatible With (ICW) --max, --nohmm, --mid and --rfam */
#define ICWMAX   "--nohmm,--mid,--rfam,--FZ,--noF1,--noF2,--noF3,--noF4,--noF6,--doF1b,--noF2b,--noF3b,--noF4b,--noF5b,--F1,--F1b,--F2,--F2b,--F3,--F3b,--F4,--F4b,--F5,--F6,--ftau,--fsums,--fqdb,--fbeta,--fnonbanded,--nocykenv,--cykenvx,--tau,--sums,--nonbanded,--rt1,--rt2,--rt3,--ns,--anonbanded,--anewbands,--envhitbias,--filcmW,--xtau"
#define ICWNOHMM "--max,--mid,--rfam,--FZ,--noF1,--noF2,--noF3,--noF4,--doF1b,--noF2b,--noF3b,--noF4b,--noF5b,--F1,--F1b,--F2,--F2b,--F3,--F3b,--F4,--F4b,--F5,--ftau,--fsums,--tau,--sums,--rt1,--rt2,--rt3,--ns,--anonbanded,--anewbands,--envhitbias,--filcmW,--xtau"
#define ICWMID   "--max,--nohmm,--rfam,--FZ,--noF1,--noF2,--noF3,--doF1b,--noF2b,--F1,--F1b,--F2,--F2b"
#define ICWRFAM  "--max,--nohmm,--mid,--FZ"
#define ICW_FZ    "--max,--nohmm,--mid,--rfam"

#if defined (HMMER_THREADS) && defined (HAVE_MPI)
#define CPUOPTS     "--mpi"
#define MPIOPTS     "--cpu"
#else
#define CPUOPTS     NULL
#define MPIOPTS     NULL
#endif

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range     toggles   reqs   incomp              help                                                      docgroup*/
  { "-h",           eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "show brief help on version and usage",                         1 },
  { "-g",           eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "configure CM for glocal alignment [default: local]",           1 },
  { "-Z",           eslARG_REAL,   FALSE, NULL, "x>0",   NULL,  NULL,  NULL,            "set database size in *Mb* to <x> for E-value calculations",    1 },
  /* Control of output */
  { "-o",           eslARG_OUTFILE, NULL, NULL, NULL,    NULL,  NULL,  NULL,            "direct output to file <f>, not stdout",                        2 },
  { "--tblout",     eslARG_OUTFILE, NULL, NULL, NULL,    NULL,  NULL,  NULL,            "save parseable table of hits to file <s>",                     2 },
  { "--acc",        eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "prefer accessions over names in output",                       2 },
  { "--noali",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "don't output alignments, so output is smaller",                2 },
  { "--notextw",    eslARG_NONE,    NULL, NULL, NULL,    NULL,  NULL, "--textw",        "unlimit ASCII text output line width",                         2 },
  { "--textw",      eslARG_INT,    "120", NULL, "n>=120",NULL,  NULL, "--notextw",      "set max width of ASCII text output lines",                     2 },
  { "--allstats",   eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "print all pipeline statistics",                                2 },
  /* Control of reporting thresholds */
  { "-E",           eslARG_REAL,  "10.0", NULL, "x>0",   NULL,  NULL,  REPOPTS,         "report sequences <= this E-value threshold in output",         3 },
  { "-T",           eslARG_REAL,   FALSE, NULL, NULL,    NULL,  NULL,  REPOPTS,         "report sequences >= this score threshold in output",           3 },
  /* Control of inclusion (significance) thresholds */
  { "--incE",       eslARG_REAL,  "0.01", NULL, "x>0",   NULL,  NULL,  INCOPTS,         "consider sequences <= this E-value threshold as significant",  4 },
  { "--incT",       eslARG_REAL,   FALSE, NULL, NULL,    NULL,  NULL,  INCOPTS,         "consider sequences >= this score threshold as significant",    4 },
  /* Model-specific thresholding for both reporting and inclusion */
  { "--cut_ga",     eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  THRESHOPTS,      "use CM's GA gathering cutoffs as reporting thresholds",        5 },
  { "--cut_nc",     eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  THRESHOPTS,      "use CM's NC noise cutoffs as reporting thresholds",            5 },
  { "--cut_tc",     eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  THRESHOPTS,      "use CM's TC trusted cutoffs as reporting thresholds",          5 },
  /* Control of filtering mode/acceleration level */
  { "--max",        eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  ICWMAX,          "turn all heuristic filters off             (power: 1st, speed: 4th)", 6 },
  { "--nohmm",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  ICWNOHMM,        "skip all HMM filter stages, use only CM    (power: 2nd, speed: 3rd)", 6 },
  { "--mid",        eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  ICWMID,          "skip first two HMM filter stages (MSV&Vit) (power: 3rd, speed: 2nd)", 6 },
  { "--rfam",       eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  ICWRFAM,         "set heuristic filters at Rfam-level        (power: 4th, speed: 1st)", 6 },
  ///{ "--hmm",        eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  FMODEOPTS,      "use only an HMM, ignore 2ary structure     (power: 5th, speed: 1st)", 6 },
  ///{ "--maxhmm",     eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  FMODEOPTS,      "use only an HMM, ignore 2ary structure     (power: 5th, speed: 1st)", 6 },
  { "--FZ",         eslARG_REAL,    NULL, NULL, NULL,    NULL,  NULL,  ICW_FZ,          "set filters to defaults used for a database of size <x> Mb",          6 },
  { "--Fmid",       eslARG_REAL,  "0.02", NULL, NULL,    NULL,"--mid", NULL,            "with --mid, set P-value threshold for HMM stages to <x>",             6 },
  /* Control of truncated hit detection */
  { "--notrunc",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "do not allow truncated hits at sequence terminii",             7 },
  { "--anytrunc",   eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,"-g,--notrunc",    "allow truncated hits anywhere within sequences",               7 },
  /* Other options */
  { "--nonull3",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "turn OFF the NULL3 post hoc additional null model",            8 },
  { "--mxsize",     eslARG_REAL,  "128.", NULL, "x>0.1", NULL,  NULL,  NULL,            "set max allowed size of DP matrices to <x> Mb",                8 },
  { "--cyk",        eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "use scanning CM CYK algorithm, not Inside in final stage",     8 },
  { "--acyk",       eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "align hits with CYK, not optimal accuracy",                    8 },
  { "--toponly",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "only search the top strand",                                   8 },
  { "--bottomonly", eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "only search the bottom strand",                                8 },
  { "--qformat",    eslARG_STRING,  NULL, NULL, NULL,    NULL,  NULL,  NULL,            "assert query <seqfile> is in format <s>: no autodetection",  12 },
#ifdef HMMER_THREADS 
  { "--cpu",        eslARG_INT, NULL,"HMMER_NCPU","n>=0",NULL,  NULL,  CPUOPTS,         "number of parallel CPU workers to use for multithreads",       8 },
#endif
#ifdef HAVE_MPI
  { "--stall",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,"--mpi", NULL,            "arrest after start: for debugging MPI under gdb",              8 },  
  { "--mpi",        eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  MPIOPTS,         "run as an MPI parallel program",                               8 },
#endif
  { "--devhelp",    eslARG_NONE,   NULL,  NULL, NULL,    NULL,  NULL,  NULL,            "show list of undocumented developer options",                  8 },

  /* All options below are developer options, only shown if --devhelp invoked */
  /* developer options controlling the acceleration pipeline */
  { "--noF1",       eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max",          "skip the MSV filter stage",                                  101 },
  { "--noF2",       eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max",          "skip the Viterbi filter stage",                              101 },
  { "--noF3",       eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max",          "skip the Forward filter stage",                              101 },
  { "--noF4",       eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max",          "skip the glocal Forward filter stage",                       101 },
  { "--noF6",       eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, NULL,             "skip the CYK filter stage",                                  101 },
  { "--doF1b",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max,--noF1",   "turn on the MSV composition bias filter",                    101 },
  { "--noF2b",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max,--noF2",   "turn off the Vit composition bias filter",                   101 },
  { "--noF3b",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max,--noF3",   "turn off the Fwd composition bias filter",                   101 },
  { "--noF4b",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max,--noF3",   "turn off the glocal Fwd composition bias filter",            101 },
  { "--noF5b",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max",          "turn off the per-envelope composition bias filter",          101 },
  { "--F1",         eslARG_REAL,  "0.35", NULL, NULL,    NULL,  NULL, "--max",          "Stage 1 (MSV) threshold:         promote hits w/ P <= <x>",  101 },
  { "--F1b",        eslARG_REAL,  "0.35", NULL, NULL,    NULL,"--doF1b", "--max",       "Stage 1 (MSV) bias threshold:    promote hits w/ P <= <x>",  101 },
  { "--F2",         eslARG_REAL,  "0.20", NULL, NULL,    NULL,  NULL, "--max",          "Stage 2 (Vit) threshold:         promote hits w/ P <= <x>",  101 },
  { "--F2b",        eslARG_REAL,  "0.20", NULL, NULL,    NULL,  NULL, "--noF2b,--max",  "Stage 2 (Vit) bias threshold:    promote hits w/ P <= <x>",  101 },
  { "--F3",         eslARG_REAL, "0.003", NULL, NULL,    NULL,  NULL, "--max",          "Stage 3 (Fwd) threshold:         promote hits w/ P <= <x>",  101 },
  { "--F3b",        eslARG_REAL, "0.003", NULL, NULL,    NULL,  NULL, "--noF3b,--max",  "Stage 3 (Fwd) bias threshold:    promote hits w/ P <= <x>",  101 },
  { "--F4",         eslARG_REAL, "0.003", NULL, NULL,    NULL,  NULL, "--max",          "Stage 4 (gFwd) glocal threshold: promote hits w/ P <= <x>",  101 },
  { "--F4b",        eslARG_REAL, "0.003", NULL, NULL,    NULL,  NULL, "--noF4b,--max",  "Stage 4 (gFwd) glocal bias thr:  promote hits w/ P <= <x>",  101 },
  { "--F5",         eslARG_REAL, "0.003", NULL, NULL,    NULL,  NULL, "--max",          "Stage 5 (env defn) threshold:    promote hits w/ P <= <x>",  101 },
  { "--F5b",        eslARG_REAL, "0.003", NULL, NULL,    NULL,  NULL, "--noF5b,--max",  "Stage 5 (env defn) bias thr:     promote hits w/ P <= <x>",  101 },
  { "--F6",         eslARG_REAL,  "1e-4", NULL, NULL,    NULL,  NULL, "--max",          "Stage 6 (CYK) threshold: promote hits w/ P <= F6",           101 },
  /* banded options for CYK filter round of searching */
  { "--ftau",       eslARG_REAL, "1e-4",  NULL, "1E-18<x<1", NULL,    NULL, "--fqdb",   "set HMM band tail loss prob for CYK filter to <x>",       102 },
  { "--fsums",      eslARG_NONE,  FALSE,  NULL, NULL,        NULL,    NULL, "--fqdb",   "w/--fhbanded use posterior sums (widens bands)",          102 },
  { "--fqdb",       eslARG_NONE,  FALSE,  NULL, NULL,        NULL,    NULL, NULL,       "use QDBs in CYK filter round, not HMM bands",             102 },
  { "--fbeta",      eslARG_REAL, "1e-7",  NULL, "1E-18<x<1", NULL,"--fqdb", NULL,       "set tail loss prob for CYK filter QDB calculation to <x>",102 },
  { "--fnonbanded", eslARG_NONE,  FALSE,  NULL, NULL,        NULL,    NULL, "--ftau,--fsums,--fqdb,--fbeta","do not use any bands for CYK filter round", 102},
  { "--nocykenv",   eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--max",          "do not redefine envelopes after stage 6 based on CYK hits",  102 },
  { "--cykenvx",    eslARG_INT,     "10", NULL, "n>=1",  NULL,  NULL, "--max",          "CYK envelope redefinition threshold multiplier, <n> * F6",   102 },
  /* banded options for final round of searching */
  { "--tau",        eslARG_REAL,"5e-6",   NULL, "1E-18<x<1", NULL,    NULL,"--qdb", "set HMM band tail loss prob for final round to <x>",        103 },
  { "--sums",       eslARG_NONE, FALSE,   NULL, NULL,        NULL,    NULL,"--qdb", "w/--hbanded use posterior sums (widens bands)",             103 },
  { "--qdb",        eslARG_NONE, FALSE,   NULL, NULL,        NULL,    NULL, NULL,   "use QDBs (instead of HMM bands) in final Inside round",           103 },
  { "--beta",       eslARG_REAL,"1e-15",  NULL, "1E-18<x<1", NULL, "--qdb",NULL,    "set tail loss prob for final Inside QDB calculation to <x>",      103 },
  { "--nonbanded",  eslARG_NONE,  FALSE,  NULL, NULL,        NULL,    NULL,"--tau,--sums,--qdb,--beta", "do not use QDBs or HMM bands in final Inside round of CM search", 103 },
  /* timing individual pipeline stages */
  { "--time-F1",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, TIMINGOPTS,       "abort after Stage 1 MSV; for timings",                       104 },
  { "--time-F2",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, TIMINGOPTS,       "abort after Stage 2 Vit; for timings",                       104 },
  { "--time-F3",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, TIMINGOPTS,       "abort after Stage 3 Fwd; for timings",                       104 },
  { "--time-F4",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, TIMINGOPTS,       "abort after Stage 4 glocal Fwd; for timings",                104 },
  { "--time-F5",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, TIMINGOPTS,       "abort after Stage 5 envelope def; for timings",              104 },
  { "--time-F6",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, TIMINGOPTS,       "abort after Stage 6 CYK; for timings",                       104 },
  /* controlling envelope definition */
  { "--rt1",        eslARG_REAL,  "0.25", NULL, NULL,    NULL,  NULL, "--nohmm,--max",  "set domain/envelope definition rt1 parameter as <x>",        105 },
  { "--rt2",        eslARG_REAL,  "0.10", NULL, NULL,    NULL,  NULL, "--nohmm,--max",  "set domain/envelope definition rt2 parameter as <x>",        105 },
  { "--rt3",        eslARG_REAL,  "0.20", NULL, NULL,    NULL,  NULL, "--nohmm,--max",  "set domain/envelope definition rt3 parameter as <x>",        105 },
  { "--ns",         eslARG_INT,   "200",  NULL, NULL,    NULL,  NULL, "--nohmm,--max",  "set number of domain/envelope tracebacks to <n>",            105 },
  /* other developer options */
  { "--anonbanded", eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "do not use HMM bands when aligning hits",                    106 },
  { "--anewbands",  eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "recalculate HMM bands for alignment, don't use scan bands",  106 },
  { "--envhitbias", eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--noedefbias",   "calc env bias for only the envelope, not entire window",    106 },
  { "--nogreedy",   eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "do not resolve hits with greedy algorithm, use optimal one", 106 },
  { "--filcmW",     eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "use CM's window length for all HMM filters",                 106 },
  { "--cp9noel",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "-g",             "turn off local ends in cp9 HMMs",                            106 },
  { "--cp9gloc",    eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  "-g,--cp9noel",  "configure cp9 HMM in glocal mode",                           106 },
  { "--null2",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "turn on null 2 biased composition score corrections",        106 },
  { "--xtau",       eslARG_REAL,    "2.", NULL, NULL,    NULL,  NULL,  NULL,            "set multiplier for tau to <x> when tightening HMM bands",    106 },
  { "--seed",       eslARG_INT,    "181", NULL, "n>=0",  NULL,  NULL,  NULL,            "set RNG seed to <n> (if 0: one-time arbitrary seed)",        106 },
#ifdef HAVE_MPI
  /* Searching only a subset of sequences in the target database, currently requires MPI b/c SSI is required */
  { "--sidx",       eslARG_INT,     NULL, NULL, "n>0",   NULL,"--mpi", NULL,            "start searching at sequence index <n> in target db SSI index", 106 },
  { "--eidx",       eslARG_INT,     NULL, NULL, "n>0",   NULL,"--mpi", NULL,            "stop  searching at sequence index <n> in target db SSI index", 106 },
#endif
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* struct cfg_s : "Global" application configuration shared by all threads/processes
 * 
 * This structure is passed to routines within main.c, as a means of semi-encapsulation
 * of shared data amongst different parallel processes (threads or MPI processes).
 */
struct cfg_s {
  char            *seqfile;           /* query sequence file                             */
  char            *cmfile;            /* database CM file                                */

  int              do_mpi;            /* TRUE if we're doing MPI parallelization         */
  int              nproc;             /* how many MPI processes, total                   */
  int              my_rank;           /* who am I, in 0..nproc-1                         */

  int64_t          Z;                /* database size, in number of models * residues    */
  int              Z_setby;          /* how Z was set: CM_ZSETBY_SSIINFO, CM_ZSETBY_OPTION, CM_ZSETBY_FILEINFO */
};

static char usage[]  = "[-options] <cmdb> <seqfile>";
static char banner[] = "search sequence(s) against a profile database";

static int  serial_master(ESL_GETOPTS *go, struct cfg_s *cfg);
static int  serial_loop  (WORKER_INFO *info, CM_FILE *cmfp);

#ifdef HMMER_THREADS
#define BLOCK_SIZE 25
static int  thread_loop(ESL_THREADS *obj, ESL_WORK_QUEUE *queue, CM_FILE *cmfp);
static void pipeline_thread(void *arg);
#endif /*HMMER_THREADS*/

#ifdef HAVE_MPI
static int  mpi_master   (ESL_GETOPTS *go, struct cfg_s *cfg);
static int  mpi_worker   (ESL_GETOPTS *go, struct cfg_s *cfg);
#endif /*HAVE_MPI*/

static void process_commandline(int argc, char **argv, ESL_GETOPTS **ret_go, char **ret_cmfile, char **ret_seqfile);
static int  output_header(FILE *ofp, const ESL_GETOPTS *go, char *cmfile, char *seqfile);

#ifdef HAVE_MPI
/* Define common tags used by the MPI master/slave processes */
#define INFERNAL_ERROR_TAG          1
#define INFERNAL_HMM_TAG            2
#define INFERNAL_SEQUENCE_TAG       3
#define INFERNAL_BLOCK_TAG          4
#define INFERNAL_PIPELINE_TAG       5
#define INFERNAL_TOPHITS_TAG        6
#define INFERNAL_HIT_TAG            7
#define INFERNAL_TERMINATING_TAG    8
#define INFERNAL_READY_TAG          9

#define MAX_BLOCK_SIZE (512*1024)

typedef struct {
  uint64_t  offset;
  uint64_t  length;
  uint64_t  count;
} MSV_BLOCK;

typedef struct {
  int        complete;
  int        size;
  int        current;
  int        last;
  MSV_BLOCK *blocks;
} BLOCK_LIST;

static void mpi_failure(char *format, ...);
static int mpi_next_block(CM_FILE *cmfp, BLOCK_LIST *list, MSV_BLOCK *block);
#endif /* HAVE_MPI */

int
main(int argc, char **argv)
{
  int              status   = eslOK;

  ESL_GETOPTS     *go  = NULL;	/* command line processing                 */
  struct cfg_s     cfg;         /* configuration data                      */

  /* Set processor specific flags */
  impl_Init();

  /* Initialize what we can in the config structure (without knowing the alphabet yet) 
   */
  cfg.cmfile     = NULL;
  cfg.seqfile    = NULL;

  cfg.do_mpi     = FALSE;	           /* this gets reset below, if we init MPI */
  cfg.nproc      = 0;		           /* this gets reset below, if we init MPI */
  cfg.my_rank    = 0;		           /* this gets reset below, if we init MPI */

  /* Initializations */
  init_ilogsum();
  FLogsumInit();
  p7_FLogsumInit();		/* we're going to use table-driven Logsum() approximations at times */
  process_commandline(argc, argv, &go, &cfg.cmfile, &cfg.seqfile);    

  /* Figure out who we are, and send control there: 
   * we might be an MPI master, an MPI worker, or a serial program.
   */
#ifdef HAVE_MPI
  /* TEMP */
  pid_t pid;
  /* get the process id */
  pid = getpid();
  printf("The process id is %d\n", pid);
  fflush(stdout);
  /* TEMP */

  /* pause the execution of the programs execution until the user has a
   * chance to attach with a debugger and send a signal to resume execution
   * i.e. (gdb) signal SIGCONT
   */
  if (esl_opt_GetBoolean(go, "--stall")) pause();

  if (esl_opt_GetBoolean(go, "--mpi")) 
    {
      cfg.do_mpi     = TRUE;
      MPI_Init(&argc, &argv);
      MPI_Comm_rank(MPI_COMM_WORLD, &(cfg.my_rank));
      MPI_Comm_size(MPI_COMM_WORLD, &(cfg.nproc));

      if (cfg.my_rank > 0)  status = mpi_worker(go, &cfg);
      else 		    status = mpi_master(go, &cfg);

      MPI_Finalize();
    }
  else
#endif /*HAVE_MPI*/
    {
      status = serial_master(go, &cfg);
    }

  esl_getopts_Destroy(go);

  return status;
}

/* serial_master()
 * The serial version of cmscan.
 * For each query sequence in <seqfile> search the database of CMs for hits.
 * 
 * A master can only return if it's successful. All errors are handled immediately and fatally with p7_Fail().
 */
static int
serial_master(ESL_GETOPTS *go, struct cfg_s *cfg)
{
  FILE            *ofp      = stdout;	         /* output file for results (default stdout)        */
  FILE            *tblfp    = NULL;		 /* output stream for tabular per-seq (--tblout)    */
  int              seqfmt   = eslSQFILE_UNKNOWN; /* format of seqfile                               */
  ESL_SQFILE      *sqfp     = NULL;              /* open seqfile                                    */
  CM_FILE         *cmfp     = NULL;		 /* open CM database file                           */
  ESL_ALPHABET    *abc      = NULL;              /* sequence alphabet                               */
  ESL_STOPWATCH   *w        = NULL;              /* timing                                          */
  ESL_STOPWATCH   *mw       = NULL;              /* timing                                          */
  ESL_SQ          *qsq      = NULL;		 /* query sequence                                  */
  int              seq_idx  = 0;                 /* index of current seq we're working with         */
  int              textw;
  int              status   = eslOK;
  int              hstatus  = eslOK;
  int              sstatus  = eslOK;
  int              i;
  int              in_rc; 

  int              ncpus    = 0;

  int              infocnt  = 0;
  WORKER_INFO     *info     = NULL;
#ifdef HMMER_THREADS
  CM_P7_OM_BLOCK  *block    = NULL;
  ESL_THREADS     *threadObj= NULL;
  ESL_WORK_QUEUE  *queue    = NULL;
#endif
  char             errbuf[eslERRBUFSIZE];

  w = esl_stopwatch_Create();
  mw = esl_stopwatch_Create();

  esl_stopwatch_Start(mw);

  if (esl_opt_GetBoolean(go, "--notextw")) textw = 0;
  else                                     textw = esl_opt_GetInteger(go, "--textw");

  /* If caller declared an input format, decode it */
  if (esl_opt_IsOn(go, "--qformat")) {
    seqfmt = esl_sqio_EncodeFormat(esl_opt_GetString(go, "--qformat"));
    if (seqfmt == eslSQFILE_UNKNOWN) p7_Fail("%s is not a recognized input sequence file format\n", esl_opt_GetString(go, "--qformat"));
  }

  /* Open the target CM database and read 1 CM, but only to get the sequence alphabet */
  status = cm_file_Open(cfg->cmfile, CMDBENV, FALSE, &cmfp, errbuf);
  if      (status == eslENOTFOUND) cm_Fail("File existence/permissions problem in trying to open CM file %s.\n%s\n", cfg->cmfile, errbuf);
  else if (status == eslEFORMAT)   cm_Fail("File format problem in trying to open CM file %s.\n%s\n",                cfg->cmfile, errbuf);
  else if (status != eslOK)        cm_Fail("Unexpected error %d in opening CM file %s.\n%s\n",               status, cfg->cmfile, errbuf);  
  if      (cmfp->do_gzip)          cm_Fail("Reading gzipped CM files is not supported");
  if      (cmfp->do_stdin)         cm_Fail("Reading CM files from stdin is not supported");
  if (! cmfp->is_pressed)          cm_Fail("Failed to open binary auxfiles for %s: use cmpress first\n",             cmfp->fname);

  hstatus = cm_file_Read(cmfp, FALSE, &abc, NULL);
  if(hstatus == eslEFORMAT)  cm_Fail("bad file format in CM file %s\n%s",           cfg->cmfile, cmfp->errbuf);
  else if (hstatus != eslOK) cm_Fail("Unexpected error in reading CMs from %s\n%s", cfg->cmfile, cmfp->errbuf); 

  /* Determine database size: default is to update it as we read target CMs */
  if(esl_opt_IsUsed(go, "-Z")) { 
    cfg->Z       = (int64_t) esl_opt_GetReal(go, "-Z");
    cfg->Z_setby = CM_ZSETBY_OPTION; 
  }
  else { 
    if(cmfp->ssi == NULL) cm_Fail("Failed to open SSI index for CM file: %s\n", cmfp->fname);
    cfg->Z = (int64_t) cmfp->ssi->nprimary;
    cfg->Z_setby = CM_ZSETBY_SSI_AND_QLENGTH; /* we will multiply Z by each query sequence length */
  }
  cm_file_Close(cmfp);

  /* Open the query sequence database */
  status = esl_sqfile_OpenDigital(abc, cfg->seqfile, seqfmt, NULL, &sqfp);
  if      (status == eslENOTFOUND) cm_Fail("Failed to open sequence file %s for reading\n",      cfg->seqfile);
  else if (status == eslEFORMAT)   cm_Fail("Sequence file %s is empty or misformatted\n",        cfg->seqfile);
  else if (status == eslEINVAL)    cm_Fail("Can't autodetect format of a stdin or .gz seqfile");
  else if (status != eslOK)        cm_Fail("Unexpected error %d opening sequence file %s\n", status, cfg->seqfile);
  qsq = esl_sq_CreateDigital(abc);

  /* Open the results output files */
  if (esl_opt_IsOn(go, "-o"))          { if ((ofp      = fopen(esl_opt_GetString(go, "-o"),          "w")) == NULL)  esl_fatal("Failed to open output file %s for writing\n",                 esl_opt_GetString(go, "-o")); }
  if (esl_opt_IsOn(go, "--tblout"))    { if ((tblfp    = fopen(esl_opt_GetString(go, "--tblout"),    "w")) == NULL)  esl_fatal("Failed to open tabular per-seq output file %s for writing\n", esl_opt_GetString(go, "--tblfp")); }
 
  output_header(ofp, go, cfg->cmfile, cfg->seqfile);

#ifdef HMMER_THREADS
  /* initialize thread data */
  if (esl_opt_IsOn(go, "--cpu")) ncpus = esl_opt_GetInteger(go, "--cpu");
  else                           esl_threads_CPUCount(&ncpus);
  printf("NCPUS: %d\n", ncpus);
  if (ncpus > 0)
    {
      threadObj = esl_threads_Create(&pipeline_thread);
      queue = esl_workqueue_Create(ncpus * 2);
    }
#endif

  infocnt = (ncpus == 0) ? 1 : ncpus;
  ESL_ALLOC(info, sizeof(*info) * infocnt);

  for (i = 0; i < infocnt; ++i)
    {
      info[i].bg    = p7_bg_Create(abc);
      info[i].in_rc = FALSE;
      ESL_ALLOC(info[i].p7_evparam, sizeof(float) * CM_p7_NEVPARAM);
#ifdef HMMER_THREADS
      info[i].queue = queue;
#endif
    }

#ifdef HMMER_THREADS
  for (i = 0; i < ncpus * 2; ++i)
    {
      block = cm_p7_oprofile_CreateBlock(BLOCK_SIZE);
      if (block == NULL)    esl_fatal("Failed to allocate sequence block");

      status = esl_workqueue_Init(queue, block);
      if (status != eslOK)  esl_fatal("Failed to add block to work queue");
    }
#endif

  /* Outside loop: over each query sequence in <seqfile>. */
  while ((sstatus = esl_sqio_Read(sqfp, qsq)) == eslOK)
    {
      seq_idx++;
      esl_stopwatch_Start(w);	                          

      fprintf(ofp, "Query:       %s  [L=%ld]\n", qsq->name, (long) qsq->n);
      if (qsq->acc[0]  != 0) fprintf(ofp, "Accession:   %s\n", qsq->acc);
      if (qsq->desc[0] != 0) fprintf(ofp, "Description: %s\n", qsq->desc);

      for (i = 0; i < infocnt; ++i)
	{
	  /* Create processing pipeline and hit list */
	  info[i].th  = cm_tophits_Create(); 
	  info[i].pli = cm_pipeline_Create(go, abc, 100, 100, cfg->Z * qsq->n, cfg->Z_setby, CM_SCAN_MODELS); /* M_hint = 100, L_hint = 100 are just dummies for now */
	  info[i].pli->nseqs++;
	  info[i].qsq = qsq;
	}

      /* scan all target CMs twice, once with the top strand of the query and once with the bottom strand */
      for(in_rc = 0; in_rc <= 1; in_rc++) { 
	if(in_rc == 0 && (! info->pli->do_top)) continue; /* skip top strand */
	if(in_rc == 1 && (! info->pli->do_bot)) continue; /* skip bottom strand */
	if(in_rc == 1) { 
	  if(qsq->abc->complement == NULL) { 
	    if(! info->pli->do_top) cm_Fail("Trying to only search bottom strand but sequence cannot be complemented"); 
	    else continue; /* skip bottom strand b/c we can't complement the sequence */
	  }
	  esl_sq_ReverseComplement(qsq);
	}

	/* open the target profile database */
	if ((status = cm_file_Open(cfg->cmfile, CMDBENV, FALSE, &cmfp, NULL)) != eslOK) cm_Fail("Unexpected error %d in opening cm file %s.\n%s", status, cfg->cmfile, cmfp->errbuf);  
#ifdef HMMER_THREADS
	if (ncpus > 0) { /* if we are threaded, create locks to prevent multiple readers */
	  if ((status = cm_file_CreateLock(cmfp)) != eslOK) cm_Fail("Unexpected error %d creating lock\n", status);
	}
#endif
	for (i = 0; i < infocnt; ++i) {
	  info[i].pli->cmfp = cmfp;                 /* for four-stage input, pipeline needs <cmfp> */
	  cm_pli_NewSeq(info[i].pli, qsq, seq_idx-1); 
	  info[i].in_rc = in_rc;
#ifdef HMMER_THREADS
	if (ncpus > 0) esl_threads_AddThread(threadObj, &info[i]);
#endif
	}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 
#ifdef HMMER_THREADS
	if (ncpus > 0)  hstatus = thread_loop(threadObj, queue, cmfp);
	else	        hstatus = serial_loop(info, cmfp);
#else
	hstatus = serial_loop(info, cmfp);
#endif
	switch(hstatus) 
	  { 
	  case eslEFORMAT:   cm_Fail("bad file format in CM file %s",             cfg->cmfile);  break;
	  case eslEINCOMPAT: cm_Fail("CM file %s contains different alphabets",   cfg->cmfile);  break;
	  case eslEMEM:      cm_Fail("Out of memory");	                                         break;
	  case eslEOF:       /* do nothing */                                                    break;
	  default:           cm_Fail("Unexpected error in reading CMs from %s",   cfg->cmfile);  break; 
	  }

	cm_file_Close(cmfp);
      } /* end of 'for(in_rc == 0; in_rc <= 1; in_rc++)' */

      /***********************************************************************/
      /* merge the results of the search results */
      for (i = 1; i < infocnt; ++i)
	{
	  cm_tophits_Merge(info[0].th, info[i].th);
	  cm_pipeline_Merge(info[0].pli, info[i].pli);

	  cm_pipeline_Destroy(info[i].pli, NULL);
	  cm_tophits_Destroy(info[i].th);
	}

      if(info[0].pli->do_top && info[0].pli->do_bot) { 
	/* we've searched all models versus each sequence then reverse
	 * complement it and search all models versus it again, so
	 * we've double counted all models.
	 */
	info[0].pli->nmodels /= 2;
	info[0].pli->nnodes  /= 2;
      }

      if(info[0].pli->do_trunc_ends) {
	/* We may have overlaps so sort by sequence index/position and remove duplicates */
	cm_tophits_SortForOverlapRemoval(info[0].th);
	if((status = cm_tophits_RemoveOverlaps(info[0].th, errbuf)) != eslOK) cm_Fail(errbuf);
      }

      /* Sort by score and enforce threshold. */
      cm_tophits_SortByScore(info->th);
      cm_tophits_Threshold(info->th, info->pli);

      /* tally up total number of hits and target coverage */
      for (i = 0; i < info->th->N; i++) {
	if ((info->th->hit[i]->flags & CM_HIT_IS_REPORTED) || (info->th->hit[i]->flags & CM_HIT_IS_INCLUDED)) { 
	  info->pli->acct[info->th->hit[i]->pass_idx].n_output++;
	  info->pli->acct[info->th->hit[i]->pass_idx].pos_output += abs(info->th->hit[i]->stop - info->th->hit[i]->start) + 1;
	}
      }
      cm_tophits_Targets(ofp, info->th, info->pli, textw); fprintf(ofp, "\n\n");
      fprintf(ofp, "\n\n");
      if(info->pli->do_alignments) {
	if((status = cm_tophits_HitAlignments(ofp, info->th, info->pli, textw)) != eslOK) esl_fatal("Out of memory");
	fprintf(ofp, "\n\n");
      }

      if (tblfp)    cm_tophits_TabularTargets(tblfp,    qsq->name, qsq->acc, info->th, info->pli, (seq_idx == 1));

      esl_stopwatch_Stop(w);
      cm_pli_Statistics(ofp, info->pli, w);
      fflush(ofp);

      cm_pipeline_Destroy(info->pli, NULL);
      cm_tophits_Destroy(info->th);
      esl_sq_Reuse(qsq);
    }
  if      (sstatus == eslEFORMAT) esl_fatal("Parse failed (sequence file %s):\n%s\n",
					    sqfp->filename, esl_sqfile_GetErrorBuf(sqfp));
  else if (sstatus != eslEOF)     esl_fatal("Unexpected error %d reading sequence file %s",
					    sstatus, sqfp->filename);

  /* Terminate outputs - any last words?
   */
  if (tblfp)    cm_tophits_TabularTail(tblfp,    "cmscan", CM_SCAN_MODELS, cfg->seqfile, cfg->cmfile, go);
  if (ofp)      fprintf(ofp, "[ok]\n");

  esl_stopwatch_Stop(mw);
  esl_stopwatch_Display(stdout, mw, "Total runtime:");

  /* Cleanup - prepare for successful exit
   */
  for (i = 0; i < infocnt; ++i)
    {
      free(info[i].p7_evparam);
      p7_bg_Destroy(info[i].bg);
    }

#ifdef HMMER_THREADS
  if (ncpus > 0)
    {
      esl_workqueue_Reset(queue);
      while (esl_workqueue_Remove(queue, (void **) &block) == eslOK)
	{
	  cm_p7_oprofile_DestroyBlock(block);
	}
      esl_workqueue_Destroy(queue);
      esl_threads_Destroy(threadObj);
    }
#endif

  free(info);

  esl_sq_Destroy(qsq);
  esl_stopwatch_Destroy(w);
  esl_stopwatch_Destroy(mw);
  esl_alphabet_Destroy(abc);
  esl_sqfile_Close(sqfp);

  if (ofp != stdout) fclose(ofp);
  if (tblfp)         fclose(tblfp);

  return eslOK;

 ERROR:
  return eslFAIL;
}

static int
serial_loop(WORKER_INFO *info, CM_FILE *cmfp)
{
  int               status;
  CM_t             *cm         = NULL; 
  ESL_ALPHABET     *abc        = NULL;
  P7_OPROFILE      *om         = NULL;      /* optimized query profile HMM            */
  P7_PROFILE       *gm         = NULL;      /* generic   query profile HMM            */
  P7_PROFILE       *Rgm        = NULL;      /* generic query profile HMM for env defn for 5' truncated hits */
  P7_PROFILE       *Lgm        = NULL;      /* generic query profile HMM for env defn for 3' truncated hits */
  P7_PROFILE       *Tgm        = NULL;      /* generic query profile HMM for env defn for 5' and 3'truncated hits */
  CMConsensus_t    *cmcons     = NULL;
  int               prv_ntophits;           /* number of top hits before cm_Pipeline() call */
  int               cm_clen, cm_W;          /* consensus, window length for current CM */        
  float             gfmu, gflambda;         /* glocal fwd mu, lambda for current hmm filter */
  off_t             cm_offset;              /* file offset for current CM */
  int64_t           cm_idx = 0;                /* index of CM we're currently working with */
  /* Main loop: */
  while ((status = cm_p7_oprofile_ReadMSV(cmfp, TRUE, &abc, &cm_offset, &cm_clen, &cm_W, &gfmu, &gflambda, &om)) == eslOK)
    {
      cm_idx++;
      esl_vec_FCopy(om->evparam, p7_NEVPARAM, info->p7_evparam);
      info->p7_evparam[CM_p7_GFMU]     = gfmu;
      info->p7_evparam[CM_p7_GFLAMBDA] = gflambda;
      gm     = NULL; /* this will get filled in cm_Pipeline() only if necessary */
      cm     = NULL; /* this will get filled in cm_Pipeline() only if necessary */
      cmcons = NULL; /* this will get filled in cm_Pipeline() only if necessary */
      if((status = cm_pli_NewModel(info->pli, CM_NEWMODEL_MSV, 
				   cm,                                   /* this is NULL b/c we don't have one yet */
				   cm_clen, cm_W,                        /* we read these in cm_p7_oprofile_ReadMSV() */
				   om, info->bg, cm_idx-1)) != eslOK) cm_Fail(info->pli->errbuf);

      prv_ntophits = info->th->N;
      if((status = cm_Pipeline(info->pli, cm_offset, om, info->bg, info->p7_evparam, NULL, info->qsq, info->th, &gm, &Rgm, &Lgm, &Tgm, &cm, &cmcons)) != eslOK)
	cm_Fail("cm_pipeline() failed unexpected with status code %d\n%s", status, info->pli->errbuf);
      cm_pipeline_Reuse(info->pli); 
      if(info->in_rc && info->th->N != prv_ntophits) cm_tophits_UpdateHitPositions(info->th, prv_ntophits, info->qsq->start, info->in_rc);

      if(info->th->N != prv_ntophits) { 
	cm_tophits_ComputeEvalues(info->th, cm->expA[info->pli->final_cm_exp_mode]->cur_eff_dbsize, prv_ntophits);
      }

      if(cmcons != NULL) { FreeCMConsensus(cmcons); cmcons = NULL; }
      if(cm     != NULL) { FreeCM(cm);              cm     = NULL; }
      if(om     != NULL) { p7_oprofile_Destroy(om); om     = NULL; }
      if(gm     != NULL) { p7_profile_Destroy(gm);  gm     = NULL; }
      if(Rgm    != NULL) { p7_profile_Destroy(Rgm); Rgm    = NULL; }
      if(Lgm    != NULL) { p7_profile_Destroy(Lgm); Lgm    = NULL; }
      if(Tgm    != NULL) { p7_profile_Destroy(Tgm); Tgm    = NULL; }
    } /* end of while(cm_p7_oprofile_ReadMSV() == eslOK) */

  esl_alphabet_Destroy(abc);

  return status;
}

#ifdef HMMER_THREADS
static int
thread_loop(ESL_THREADS *obj, ESL_WORK_QUEUE *queue, CM_FILE *cmfp)
{
  int  status   = eslOK;
  int  sstatus  = eslOK;
  int  eofCount = 0;
  CM_P7_OM_BLOCK  *block;
  ESL_ALPHABET    *abc = NULL;
  void            *newBlock;

  esl_workqueue_Reset(queue);
  esl_threads_WaitForStart(obj);

  status = esl_workqueue_ReaderUpdate(queue, NULL, &newBlock);
  if (status != eslOK) esl_fatal("Work queue reader failed");
      
  /* Main loop: */
  while (sstatus == eslOK)
    {
      block = (CM_P7_OM_BLOCK *) newBlock;
      sstatus = cm_p7_oprofile_ReadBlockMSV(cmfp, &abc, block);
      if (sstatus == eslEOF) { 
	if (eofCount < esl_threads_GetWorkerCount(obj)) sstatus = eslOK;
	++eofCount;
      }
      if (sstatus == eslOK) { 
	status = esl_workqueue_ReaderUpdate(queue, block, &newBlock);
	if (status != eslOK) esl_fatal("Work queue reader failed");
      }
    }

  status = esl_workqueue_ReaderUpdate(queue, block, NULL);
  if (status != eslOK) esl_fatal("Work queue reader failed");

  if (sstatus == eslEOF)
    {
      /* wait for all the threads to complete */
      esl_threads_WaitForFinish(obj);
      esl_workqueue_Complete(queue);  
    }
  
  esl_alphabet_Destroy(abc);
  return sstatus;
}

static void
pipeline_thread(void *arg)
{
  int i;
  int status;
  int workeridx;
  WORKER_INFO     *info;
  ESL_THREADS     *obj;
  CM_P7_OM_BLOCK  *block;
  void            *newBlock;

  CM_t             *cm         = NULL; 
  ESL_ALPHABET     *abc        = NULL;
  P7_PROFILE       *gm         = NULL;      /* generic   query profile HMM            */
  P7_PROFILE       *Rgm        = NULL;      /* generic query profile HMM for env defn for 5' truncated hits */
  P7_PROFILE       *Lgm        = NULL;      /* generic query profile HMM for env defn for 3' truncated hits */
  P7_PROFILE       *Tgm        = NULL;      /* generic query profile HMM for env defn for 5' and 3'truncated hits */
  CMConsensus_t    *cmcons     = NULL;
  int               prv_ntophits;           /* number of top hits before cm_Pipeline() call */
  int               cm_clen, cm_W;          /* consensus, window length for current CM */        
  float             gfmu, gflambda;         /* glocal fwd mu, lambda for current hmm filter */
  off_t             cm_offset;              /* file offset for current CM */
  int64_t           cm_idx = 0;             /* index of CM we're currently working with */
  
#ifdef HAVE_FLUSH_ZERO_MODE
  /* In order to avoid the performance penalty dealing with sub-normal
   * values in the floating point calculations, set the processor flag
   * so sub-normals are "flushed" immediately to zero.
   * On OS X, need to reset this flag for each thread
   * (see TW notes 05/08/10 for details)
   */
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif

  obj = (ESL_THREADS *) arg;
  esl_threads_Started(obj, &workeridx);

  info = (WORKER_INFO *) esl_threads_GetData(obj, workeridx);

  status = esl_workqueue_WorkerUpdate(info->queue, NULL, &newBlock);
  if (status != eslOK) esl_fatal("Work queue worker failed");

  /* loop until all blocks have been processed */
  block = (CM_P7_OM_BLOCK *) newBlock;
  while (block->count > 0)
    {
      /* Main loop: */
      for (i = 0; i < block->count; ++i)
	{
	  P7_OPROFILE *om = block->list[i];
	  cm_offset       = block->cm_offsetA[i];
	  cm_clen         = block->cm_clenA[i];
	  cm_W            = block->cm_WA[i];
	  gfmu            = block->gfmuA[i];
	  gflambda        = block->gflambdaA[i];
	  cm_idx++;

	  esl_vec_FCopy(om->evparam, p7_NEVPARAM, info->p7_evparam);
	  info->p7_evparam[CM_p7_GFMU]     = gfmu;
	  info->p7_evparam[CM_p7_GFLAMBDA] = gflambda;
	  gm     = NULL; /* this will get filled in cm_Pipeline() only if necessary */
	  cm     = NULL; /* this will get filled in cm_Pipeline() only if necessary */
	  cmcons = NULL; /* this will get filled in cm_Pipeline() only if necessary */
	  if((status = cm_pli_NewModel(info->pli, CM_NEWMODEL_MSV, 
				       cm,                                   /* this is NULL b/c we don't have one yet */
				       cm_clen, cm_W,                        /* we read these in cm_p7_oprofile_ReadMSV() */
				       om, info->bg, cm_idx-1)) != eslOK) cm_Fail(info->pli->errbuf);

	  prv_ntophits = info->th->N;
	  if((status = cm_Pipeline(info->pli, cm_offset, om, info->bg, info->p7_evparam, NULL, info->qsq, info->th, &gm, &Rgm, &Lgm, &Tgm, &cm, &cmcons)) != eslOK)
	    cm_Fail("cm_pipeline() failed unexpected with status code %d\n%s", status, info->pli->errbuf);
	  cm_pipeline_Reuse(info->pli);
	  if(info->in_rc && info->th->N != prv_ntophits) cm_tophits_UpdateHitPositions(info->th, prv_ntophits, info->qsq->start, info->in_rc);

	  if(info->th->N != prv_ntophits) { 
	    cm_tophits_ComputeEvalues(info->th, cm->expA[info->pli->final_cm_exp_mode]->cur_eff_dbsize, prv_ntophits);
	  }
		
	  if(cmcons != NULL) { FreeCMConsensus(cmcons); cmcons = NULL; }
	  if(cm     != NULL) { FreeCM(cm);              cm     = NULL; }
	  if(om     != NULL) { p7_oprofile_Destroy(om); om     = NULL; }
	  if(gm     != NULL) { p7_profile_Destroy(gm);  gm     = NULL; }
	  if(Rgm    != NULL) { p7_profile_Destroy(Rgm); Rgm    = NULL; }
	  if(Lgm    != NULL) { p7_profile_Destroy(Lgm); Lgm    = NULL; }
	  if(Tgm    != NULL) { p7_profile_Destroy(Tgm); Tgm    = NULL; }
	  
	  block->list[i] = NULL;
	  block->cm_offsetA[i] = 0;
	  block->cm_clenA[i]   = 0;
	  block->cm_WA[i]      = 0;
	  block->gfmuA[i]      = 0.;
	  block->gflambdaA[i]  = 0.;
	}
      
      status = esl_workqueue_WorkerUpdate(info->queue, block, &newBlock);
      if (status != eslOK) esl_fatal("Work queue worker failed");
      
      block = (CM_P7_OM_BLOCK *) newBlock;
    }
  
  status = esl_workqueue_WorkerUpdate(info->queue, block, NULL);
  if (status != eslOK) esl_fatal("Work queue worker failed");

  esl_threads_Finished(obj, workeridx);

  esl_alphabet_Destroy(abc);

  return;
}
#endif   /* HMMER_THREADS */

#if HAVE_MPI
/* mpi_master()
 * The MPI version of cmscan
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
 * cm_Fail()'s, which will cause MPI to shut down the worker processes
 * uncleanly.
 */
static int
mpi_master(ESL_GETOPTS *go, struct cfg_s *cfg)
{
  FILE            *ofp      = stdout;	         /* output file for results (default stdout)        */
  FILE            *tblfp    = NULL;		 /* output stream for tabular per-seq (--tblout)    */
  int              seqfmt   = eslSQFILE_UNKNOWN; /* format of seqfile                               */
  P7_BG           *bg       = NULL;	         /* null model                                      */
  ESL_SQFILE      *sqfp     = NULL;              /* open seqfile                                    */
  CM_FILE         *cmfp     = NULL;		 /* open CMM database file                          */
  ESL_ALPHABET    *abc      = NULL;              /* sequence alphabet                               */
  ESL_STOPWATCH   *w        = NULL;              /* timing                                          */
  ESL_STOPWATCH   *mw       = NULL;              /* timing                                          */
  ESL_SQ          *qsq      = NULL;		 /* query sequence                                  */
  int              seq_idx   = 0;
  int              textw;
  int              status   = eslOK;
  int              hstatus  = eslOK;
  int              sstatus  = eslOK;
  int              dest;

  char            *mpi_buf  = NULL;              /* buffer used to pack/unpack structures */
  int              mpi_size = 0;                 /* size of the allocated buffer */
  BLOCK_LIST      *list     = NULL;
  MSV_BLOCK        block;

  int              i;
  int              size;
  MPI_Status       mpistatus;
  char             errbuf[eslERRBUFSIZE];
  int              in_rc; 

  w = esl_stopwatch_Create();
  mw = esl_stopwatch_Create();

  esl_stopwatch_Start(mw);
  
  if (esl_opt_GetBoolean(go, "--notextw")) textw = 0;
  else                                     textw = esl_opt_GetInteger(go, "--textw");

  /* If caller declared an input format, decode it */
  if (esl_opt_IsOn(go, "--qformat")) {
    seqfmt = esl_sqio_EncodeFormat(esl_opt_GetString(go, "--qformat"));
    if (seqfmt == eslSQFILE_UNKNOWN) mpi_failure("%s is not a recognized input sequence file format\n", esl_opt_GetString(go, "--qformat"));
  }

  /* Open the target profile database to get the sequence alphabet */
  status = cm_file_Open(cfg->cmfile, CMDBENV, FALSE, &cmfp, errbuf);
  if      (status == eslENOTFOUND) mpi_failure("File existence/permissions problem in trying to open CM file %s.\n%s\n", cfg->cmfile, errbuf);
  else if (status == eslEFORMAT)   mpi_failure("File format problem in trying to open CM file %s.\n%s\n",                cfg->cmfile, errbuf);
  else if (status != eslOK)        mpi_failure("Unexpected error %d in opening CM file %s.\n%s\n",               status, cfg->cmfile, errbuf);  
  if      (cmfp->do_gzip)          mpi_failure("Reading gzipped CM files is not supported");
  if      (cmfp->do_stdin)         mpi_failure("Reading CM files from stdin is not supported");
  if (! cmfp->is_pressed)          mpi_failure("Failed to open binary auxfiles for %s: use cmpress first\n",             cmfp->fname);

  hstatus = cm_file_Read(cmfp, FALSE, &abc, NULL);
  if(hstatus == eslEFORMAT)  mpi_failure("bad file format in CM file %s\n%s",           cfg->cmfile, cmfp->errbuf);
  else if (hstatus != eslOK) mpi_failure("Unexpected error in reading CMs from %s\n%s", cfg->cmfile, cmfp->errbuf); 

  /* Determine database size: default is to updated as we read target CMs */
  if(esl_opt_IsUsed(go, "-Z")) { 
    cfg->Z       = (int64_t) esl_opt_GetReal(go, "-Z");
    cfg->Z_setby = CM_ZSETBY_OPTION; 
  }
  else { 
    if(cmfp->ssi == NULL) mpi_failure("Failed to open SSI index for CM file: %s\n", cmfp->fname);
    cfg->Z = (int64_t) cmfp->ssi->nprimary;
    cfg->Z_setby = CM_ZSETBY_SSI_AND_QLENGTH; /* we will multiply Z by each query sequence length */
  }
  cm_file_Close(cmfp);

  /* Open the query sequence database */
  status = esl_sqfile_OpenDigital(abc, cfg->seqfile, seqfmt, NULL, &sqfp);
  if      (status == eslENOTFOUND) mpi_failure("Failed to open sequence file %s for reading\n",      cfg->seqfile);
  else if (status == eslEFORMAT)   mpi_failure("Sequence file %s is empty or misformatted\n",        cfg->seqfile);
  else if (status == eslEINVAL)    mpi_failure("Can't autodetect format of a stdin or .gz seqfile");
  else if (status != eslOK)        mpi_failure("Unexpected error %d opening sequence file %s\n", status, cfg->seqfile);

  /* Open the results output files */
  if (esl_opt_IsOn(go, "-o")          && (ofp      = fopen(esl_opt_GetString(go, "-o"),          "w")) == NULL)
    mpi_failure("Failed to open output file %s for writing\n",                 esl_opt_GetString(go, "-o"));
  if (esl_opt_IsOn(go, "--tblout")    && (tblfp    = fopen(esl_opt_GetString(go, "--tblout"),    "w")) == NULL)
    mpi_failure("Failed to open tabular per-seq output file %s for writing\n", esl_opt_GetString(go, "--tblout"));
 
  ESL_ALLOC(list, sizeof(MSV_BLOCK));
  list->complete = 0;
  list->size     = 0;
  list->current  = 0;
  list->last     = 0;
  list->blocks   = NULL;

  output_header(ofp, go, cfg->cmfile, cfg->seqfile);
  qsq = esl_sq_CreateDigital(abc);
  bg  = p7_bg_Create(abc);

  /* Outside loop: over each query sequence in <seqfile>. */
  while ((sstatus = esl_sqio_Read(sqfp, qsq)) == eslOK)
    {
      CM_PIPELINE     *pli     = NULL;		/* processing pipeline                      */
      CM_TOPHITS      *th      = NULL;        	/* top-scoring sequence hits                */

      seq_idx++;
      esl_stopwatch_Start(w);	                          

      /* seqfile may need to be rewound (multiquery mode) */
      if (seq_idx > 1) list->current = 0;

      fprintf(ofp, "Query:       %s  [L=%ld]\n", qsq->name, (long) qsq->n);
      if (qsq->acc[0]  != 0) fprintf(ofp, "Accession:   %s\n", qsq->acc);
      if (qsq->desc[0] != 0) fprintf(ofp, "Description: %s\n", qsq->desc);

      /* Create processing pipeline and hit list */
      th  = cm_tophits_Create(); 
      pli = cm_pipeline_Create(go, abc, 100, 100, cfg->Z * qsq->n, cfg->Z_setby, CM_SCAN_MODELS); /* M_hint = 100, L_hint = 100 are just dummies for now */

      /* scan all target CMs twice, once with the top strand of the query and once with the bottom strand */
      for(in_rc = 0; in_rc <= 1; in_rc++) { 
	if(in_rc == 0 && (! pli->do_top)) continue; /* skip top strand */
	if(in_rc == 1 && (! pli->do_bot)) continue; /* skip bottom strand */
	if(in_rc == 1) { 
	  if(qsq->abc->complement == NULL) { 
	    if(! pli->do_top) mpi_failure("Trying to only search bottom strand but sequence cannot be complemented"); 
	    else continue; /* skip bottom strand b/c we can't complement the sequence */
	  }
	  /* no need to complement the sequence, we only use its name, acc, desc */
	}
	
	/* Open the target profile database */
	status = cm_file_Open(cfg->cmfile, CMDBENV, FALSE, &cmfp, errbuf);
	if (status != eslOK) mpi_failure("Unexpected error %d in opening cm file %s.\n%s", status, cfg->cmfile, errbuf);  
	pli->cmfp = cmfp;  /* for four-stage input, pipeline needs <cmfp> */
	
	cm_pli_NewSeq(pli, qsq, seq_idx);

	/* Main loop: */
	while ((hstatus = mpi_next_block(cmfp, list, &block)) == eslOK)
	{
	  if (MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &mpistatus) != 0) 
	    mpi_failure("MPI error %d receiving message from %d\n", mpistatus.MPI_SOURCE);
	  
	  MPI_Get_count(&mpistatus, MPI_PACKED, &size);
	  if (mpi_buf == NULL || size > mpi_size) {
	    void *tmp;
	    ESL_RALLOC(mpi_buf, tmp, sizeof(char) * size);
	    mpi_size = size; 
	  }
	  
	  dest = mpistatus.MPI_SOURCE;
	  MPI_Recv(mpi_buf, size, MPI_PACKED, dest, mpistatus.MPI_TAG, MPI_COMM_WORLD, &mpistatus);
	  
	  if (mpistatus.MPI_TAG == INFERNAL_ERROR_TAG)
	    mpi_failure("MPI client %d raised error:\n%s\n", dest, mpi_buf);
	  if (mpistatus.MPI_TAG != INFERNAL_READY_TAG)
	    mpi_failure("Unexpected tag %d from %d\n", mpistatus.MPI_TAG, dest);
	  
	  MPI_Send(&block, 3, MPI_LONG_LONG_INT, dest, INFERNAL_BLOCK_TAG, MPI_COMM_WORLD);
	}
	switch(hstatus)
	  {
	  case eslEFORMAT:   mpi_failure("bad file format in CM file %s",           cfg->cmfile); break;
	  case eslEINCOMPAT: mpi_failure("CM file %s contains different alphabets", cfg->cmfile); break;
	  case eslEOF: 	   /* do nothing */	                                                break;
	  default:	   mpi_failure("Unexpected error %d in reading CMs from %s", hstatus, cfg->cmfile); break;
	  }
	
	block.offset = 0;
	block.length = 0;
	block.count  = 0;
	
	/* wait for all workers to finish up their work blocks */
	for (i = 1; i < cfg->nproc; ++i)
	  {
	    if (MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &mpistatus) != 0) 
	      mpi_failure("MPI error %d receiving message from %d\n", mpistatus.MPI_SOURCE);
	    
	    MPI_Get_count(&mpistatus, MPI_PACKED, &size);
	    if (mpi_buf == NULL || size > mpi_size) {
	      void *tmp;
	      ESL_RALLOC(mpi_buf, tmp, sizeof(char) * size);
	      mpi_size = size; 
	    }
	    
	    dest = mpistatus.MPI_SOURCE;
	    MPI_Recv(mpi_buf, size, MPI_PACKED, dest, mpistatus.MPI_TAG, MPI_COMM_WORLD, &mpistatus);
	    
	    if (mpistatus.MPI_TAG == INFERNAL_ERROR_TAG)
	      mpi_failure("MPI client %d raised error:\n%s\n", dest, mpi_buf);
	    if (mpistatus.MPI_TAG != INFERNAL_READY_TAG)
	      mpi_failure("Unexpected tag %d from %d\n", mpistatus.MPI_TAG, dest);
	  }

	for (dest = 1; dest < cfg->nproc; ++dest)
	  /* send an empty block to signal the worker they are done with this strand of this sequence */
	  MPI_Send(&block, 3, MPI_LONG_LONG_INT, dest, INFERNAL_BLOCK_TAG, MPI_COMM_WORLD);
      } /* end of (in_rc = 0..1) loop (in top or bottom strand) */

      /* receive and merge the results */
      for (dest = 1; dest < cfg->nproc; ++dest)
	{
	  CM_PIPELINE     *mpi_pli   = NULL;
	  CM_TOPHITS      *mpi_th    = NULL;

	  /* wait for the results */
	  if ((status = cm_tophits_MPIRecv(dest, INFERNAL_TOPHITS_TAG, MPI_COMM_WORLD, &mpi_buf, &mpi_size, &mpi_th)) != eslOK)
	    mpi_failure("Unexpected error %d receiving tophits from %d", status, dest);

	  if ((status = cm_pipeline_MPIRecv(dest, INFERNAL_PIPELINE_TAG, MPI_COMM_WORLD, &mpi_buf, &mpi_size, go, &mpi_pli)) != eslOK)
	    mpi_failure("Unexpected error %d receiving pipeline from %d", status, dest);

	  cm_tophits_Merge(th, mpi_th);
	  cm_pipeline_Merge(pli, mpi_pli);
	  
	  cm_pipeline_Destroy(mpi_pli, NULL);
	  cm_tophits_Destroy(mpi_th);
	}
      
      if(pli->do_trunc_ends) { 
	/* We may have overlaps so sort by sequence index/position and remove duplicates */
	cm_tophits_SortForOverlapRemoval(th);
	if((status = cm_tophits_RemoveOverlaps(th, errbuf)) != eslOK) mpi_failure(errbuf);
      }
      
      /* Print the results.  */
      cm_tophits_SortByScore(th);
      cm_tophits_Threshold(th, pli);

      /* tally up total number of hits and target coverage */
      for (i = 0; i < th->N; i++) {
	if ((th->hit[i]->flags & CM_HIT_IS_REPORTED) || (th->hit[i]->flags & CM_HIT_IS_INCLUDED)) { 
	  pli->acct[th->hit[i]->pass_idx].n_output++;
	  pli->acct[th->hit[i]->pass_idx].pos_output += abs(th->hit[i]->stop - th->hit[i]->start) + 1;
	}
      }

      cm_tophits_Targets(ofp, th, pli, textw); fprintf(ofp, "\n\n");
      if(pli->do_alignments) {
	if((status = cm_tophits_HitAlignments(ofp, th, pli, textw)) != eslOK) mpi_failure("Out of memory");
	fprintf(ofp, "\n\n");
      }
      
      if (tblfp)    cm_tophits_TabularTargets(tblfp,    qsq->name, qsq->acc, th, pli, (seq_idx == 1));
      
      esl_stopwatch_Stop(w);
      cm_pli_Statistics(ofp, pli, w);

      cm_file_Close(cmfp);
      cm_pipeline_Destroy(pli, NULL);
      cm_tophits_Destroy(th);
      esl_sq_Reuse(qsq);
    }
  if (sstatus == eslEFORMAT) 
    mpi_failure("Parse failed (sequence file %s):\n%s\n", sqfp->filename, esl_sqfile_GetErrorBuf(sqfp));
  else if (sstatus != eslEOF)     
    mpi_failure("Unexpected error %d reading sequence file %s", sstatus, sqfp->filename);
  
  /* monitor all the workers to make sure they have ended */
  for (i = 1; i < cfg->nproc; ++i)
    {
      if (MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &mpistatus) != 0) 
	mpi_failure("MPI error %d receiving message from %d\n", mpistatus.MPI_SOURCE);

      MPI_Get_count(&mpistatus, MPI_PACKED, &size);
      if (mpi_buf == NULL || size > mpi_size) {
	void *tmp;
	ESL_RALLOC(mpi_buf, tmp, sizeof(char) * size);
	mpi_size = size; 
      }
      
      dest = mpistatus.MPI_SOURCE;
      MPI_Recv(mpi_buf, size, MPI_PACKED, dest, mpistatus.MPI_TAG, MPI_COMM_WORLD, &mpistatus);
      
      if (mpistatus.MPI_TAG == INFERNAL_ERROR_TAG)
	mpi_failure("MPI client %d raised error:\n%s\n", dest, mpi_buf);
      if (mpistatus.MPI_TAG != INFERNAL_TERMINATING_TAG)
	mpi_failure("Unexpected tag %d from %d\n", mpistatus.MPI_TAG, dest);
    }
  
  /* Terminate outputs - any last words?
   */
  if (tblfp)    cm_tophits_TabularTail(tblfp,    "cmscan", CM_SCAN_MODELS, cfg->seqfile, cfg->cmfile, go);
  if (ofp)      fprintf(ofp, "[ok]\n");

  esl_stopwatch_Stop(mw);
  esl_stopwatch_Display(stdout, mw, "Total runtime:");
  
  /* Cleanup - prepare for successful exit
   */
  free(list);
  if (mpi_buf != NULL) free(mpi_buf);

  p7_bg_Destroy(bg);

  esl_sq_Destroy(qsq);
  esl_stopwatch_Destroy(w);
  esl_stopwatch_Destroy(mw);
  esl_alphabet_Destroy(abc);
  esl_sqfile_Close(sqfp);

  if (ofp != stdout) fclose(ofp);
  if (tblfp)         fclose(tblfp);

  return eslOK;

 ERROR:
  return eslFAIL;
}

static int
mpi_worker(ESL_GETOPTS *go, struct cfg_s *cfg)
{
  int              seqfmt   = eslSQFILE_UNKNOWN; /* format of seqfile                               */
  P7_BG           *bg       = NULL;	         /* null model                                      */
  ESL_SQFILE      *sqfp     = NULL;              /* open seqfile                                    */
  CM_FILE         *cmfp     = NULL;		 /* open CM database file                           */
  CM_t            *cm       = NULL;              /* the CM                                          */
  CMConsensus_t   *cmcons   = NULL;              /* CM consensus information                        */
  ESL_ALPHABET    *abc      = NULL;              /* sequence alphabet                               */
  P7_OPROFILE     *om       = NULL;		 /* target profile                                  */
  P7_PROFILE      *gm       = NULL;              /* generic query profile HMM                       */
  P7_PROFILE      *Rgm      = NULL;              /* generic query profile HMM for env defn for 5' truncated hits */
  P7_PROFILE      *Lgm      = NULL;              /* generic query profile HMM for env defn for 3' truncated hits */
  P7_PROFILE      *Tgm      = NULL;              /* generic query profile HMM for env defn for 5' and 3'truncated hits */
  ESL_STOPWATCH   *w        = NULL;              /* timing                                          */
  ESL_SQ          *qsq      = NULL;		 /* query sequence                                  */
  int              status   = eslOK;
  int              hstatus  = eslOK;
  int              sstatus  = eslOK;
  int              cm_clen, cm_W;          /* consensus, window length for current CM */        
  float            gfmu, gflambda;         /* glocal fwd mu, lambda for current hmm filter */
  off_t            cm_offset;              /* file offset for current CM */
  float           *p7_evparam;             /* E-value parameters for the p7 filter */
  int              prv_ntophits;           /* number of top hits before cm_Pipeline() call */
  int              in_rc;                  /* in_rc == TRUE; our qsq has been reverse complemented */

  char            *mpi_buf  = NULL;              /* buffer used to pack/unpack structures */
  int              mpi_size = 0;                 /* size of the allocated buffer */
  int              seq_idx  = 0;                 /* index of sequence we're currently working on */
  int              cm_idx   = 0;                 /* index of model    we're currently working on */

  MPI_Status       mpistatus;
  char             errbuf[eslERRBUFSIZE];

  w = esl_stopwatch_Create();

  /* Open the target profile database to get the sequence alphabet */
  status = cm_file_Open(cfg->cmfile, CMDBENV, FALSE, &cmfp, errbuf);
  if      (status == eslENOTFOUND) mpi_failure("File existence/permissions problem in trying to open CM file %s.\n%s\n", cfg->cmfile, errbuf);
  else if (status == eslEFORMAT)   mpi_failure("File format problem in trying to open CM file %s.\n%s\n",                cfg->cmfile, errbuf);
  else if (status != eslOK)        mpi_failure("Unexpected error %d in opening CM file %s.\n%s\n",               status, cfg->cmfile, errbuf);  
  if      (cmfp->do_gzip)          mpi_failure("Reading gzipped CM files is not supported");
  if      (cmfp->do_stdin)         mpi_failure("Reading CM files from stdin is not supported");
  if (! cmfp->is_pressed)          mpi_failure("Failed to open binary auxfiles for %s: use cmpress first\n",             cmfp->fname);

  hstatus = cm_file_Read(cmfp, FALSE, &abc, NULL);
  if(hstatus == eslEFORMAT)  mpi_failure("bad file format in CM file %s\n%s",           cfg->cmfile, cmfp->errbuf);
  else if (hstatus != eslOK) mpi_failure("Unexpected error in reading CMs from %s\n%s", cfg->cmfile, cmfp->errbuf); 

  /* Determine database size: default is to updated as we read target CMs */
  if(esl_opt_IsUsed(go, "-Z")) { 
    cfg->Z       = (int64_t) esl_opt_GetReal(go, "-Z");
    cfg->Z_setby = CM_ZSETBY_OPTION; 
  }
  else { 
    if(cmfp->ssi == NULL) mpi_failure("Failed to open SSI index for CM file: %s\n", cmfp->fname);
    cfg->Z = (int64_t) cmfp->ssi->nprimary;
    cfg->Z_setby = CM_ZSETBY_SSI_AND_QLENGTH; /* we will multiply Z by each query sequence length */
  }
  cm_file_Close(cmfp);

  /* Open the query sequence database */
  status = esl_sqfile_OpenDigital(abc, cfg->seqfile, seqfmt, NULL, &sqfp);
  if      (status == eslENOTFOUND) mpi_failure("Failed to open sequence file %s for reading\n",      cfg->seqfile);
  else if (status == eslEFORMAT)   mpi_failure("Sequence file %s is empty or misformatted\n",        cfg->seqfile);
  else if (status == eslEINVAL)    mpi_failure("Can't autodetect format of a stdin or .gz seqfile");
  else if (status != eslOK)        mpi_failure("Unexpected error %d opening sequence file %s\n", status, cfg->seqfile);

  qsq = esl_sq_CreateDigital(abc);
  bg  = p7_bg_Create(abc);

  ESL_ALLOC(p7_evparam, sizeof(float) * CM_p7_NEVPARAM);

  /* Outside loop: over each query sequence in <seqfile>. */
  while ((sstatus = esl_sqio_Read(sqfp, qsq)) == eslOK)
    {
      CM_PIPELINE     *pli     = NULL;		/* processing pipeline                      */
      CM_TOPHITS      *th      = NULL;        	/* top-scoring sequence hits                */
      MSV_BLOCK        block;

      seq_idx++;
      esl_stopwatch_Start(w);

      /* Create processing pipeline and hit list */
      th  = cm_tophits_Create(); 
      pli = cm_pipeline_Create(go, abc, 100, 100, cfg->Z * qsq->n, cfg->Z_setby, CM_SCAN_MODELS); /* M_hint = 100, L_hint = 100 are just dummies for now */

      /* scan all target CMs twice, once with the top strand of the query and once with the bottom strand */
      for(in_rc = 0; in_rc <= 1; in_rc++) { 
	if(in_rc == 0 && (! pli->do_top)) continue; /* skip top strand */
	if(in_rc == 1 && (! pli->do_bot)) continue; /* skip bottom strand */
	if(in_rc == 1) { 
	  if(qsq->abc->complement == NULL) { 
	    if(! pli->do_top) mpi_failure("Trying to only search bottom strand but sequence cannot be complemented"); 
	    else continue; /* skip bottom strand b/c we can't complement the sequence */
	  }
	  esl_sq_ReverseComplement(qsq);
	}

	status = 0;
	MPI_Send(&status, 1, MPI_INT, 0, INFERNAL_READY_TAG, MPI_COMM_WORLD);
	
	/* Open the target profile database */
	status = cm_file_Open(cfg->cmfile, CMDBENV, FALSE, &cmfp, errbuf);
	if (status != eslOK) mpi_failure("Unexpected error %d in opening cm file %s.\n%s", status, cfg->cmfile, errbuf);  
	pli->cmfp = cmfp;  /* for four-stage input, pipeline needs <cmfp> */
	
	cm_pli_NewSeq(pli, qsq, seq_idx);

	/* receive a block of models from the master */
	MPI_Recv(&block, 3, MPI_LONG_LONG_INT, 0, INFERNAL_BLOCK_TAG, MPI_COMM_WORLD, &mpistatus);
	while (block.count > 0)
	  {
	    uint64_t length = 0;
	    uint64_t count  = block.count;
	    
	    hstatus = cm_p7_oprofile_Position(cmfp, block.offset);
	    if (hstatus != eslOK) mpi_failure("Cannot position optimized model to %ld\n", block.offset);
	    
	    while (count > 0 && 
		   (hstatus = cm_p7_oprofile_ReadMSV(cmfp, TRUE, &abc, &cm_offset, &cm_clen, &cm_W, &gfmu, &gflambda, &om)) == eslOK)
	      {
		cm_idx++;
		length = om->eoff - block.offset + 1;
		
		esl_vec_FCopy(om->evparam, p7_NEVPARAM, p7_evparam);
		p7_evparam[CM_p7_GFMU]     = gfmu;
		p7_evparam[CM_p7_GFLAMBDA] = gflambda;
		gm     = NULL; /* this will get filled in cm_Pipeline() only if necessary */
		Rgm    = NULL; /* this will get filled in cm_Pipeline() only if necessary */
		Lgm    = NULL; /* this will get filled in cm_Pipeline() only if necessary */
		Tgm    = NULL; /* this will get filled in cm_Pipeline() only if necessary */
		cm     = NULL; /* this will get filled in cm_Pipeline() only if necessary */
		cmcons = NULL; /* this will get filled in cm_Pipeline() only if necessary */
		if((status = cm_pli_NewModel(pli, CM_NEWMODEL_MSV, 
					     cm,                                   /* this is NULL b/c we don't have one yet */
					     cm_clen, cm_W,                        /* we read these in cm_p7_oprofile_ReadMSV() */
					     om, bg, cm_idx-1)) != eslOK) mpi_failure(pli->errbuf);
		
		prv_ntophits = th->N;

		if((status = cm_Pipeline(pli, cm_offset, om, bg, p7_evparam, NULL, qsq, th, &gm, &Rgm, &Lgm, &Tgm, &cm, &cmcons)) != eslOK)
		  mpi_failure("cm_pipeline() failed unexpected with status code %d\n%s", status, pli->errbuf);
		cm_pipeline_Reuse(pli);
		if(in_rc && th->N != prv_ntophits) cm_tophits_UpdateHitPositions(th, prv_ntophits, qsq->start, in_rc);

		if(th->N != prv_ntophits) { 
		  cm_tophits_ComputeEvalues(th, cm->expA[pli->final_cm_exp_mode]->cur_eff_dbsize, prv_ntophits);
		}

		if(cmcons != NULL) { FreeCMConsensus(cmcons); cmcons = NULL; }
		if(cm     != NULL) { FreeCM(cm);              cm     = NULL; }
		if(om     != NULL) { p7_oprofile_Destroy(om); om     = NULL; }
		if(gm     != NULL) { p7_profile_Destroy(gm);  gm     = NULL; }
		if(Rgm    != NULL) { p7_profile_Destroy(Rgm); Rgm    = NULL; }
		if(Lgm    != NULL) { p7_profile_Destroy(Lgm); Lgm    = NULL; }
		if(Tgm    != NULL) { p7_profile_Destroy(Tgm); Tgm    = NULL; }
		
		--count;
	      }
	    /* check the status of reading the msv filter */
	    if (count > 0)              
	      {
		switch(hstatus)
		  {
		  case eslEFORMAT:    mpi_failure("bad file format in HMM file %s\n%s",          cfg->cmfile, cmfp->errbuf); break;
		  case eslEINCOMPAT:  mpi_failure("HMM file %s contains different alphabets",    cfg->cmfile); break;
		  case eslOK:         
		  case eslEOF:        mpi_failure("Block count mismatch - expected %ld found %ld at offset %ld\n", block.count, block.count-count, block.offset); break;
		  default:  	      mpi_failure("Unexpected error %d in reading HMMs from %s\n%s", hstatus, cfg->cmfile, cmfp->errbuf); break;
		  }
	      }
	    
	    /* lets do a little bit of sanity checking here to make sure the blocks are the same */
	    if (block.length != length) mpi_failure("Block length mismatch - expected %ld found %ld at offset %ld\n", block.length, length, block.offset);
	    
	    /* inform the master we need another block of sequences */
	    status = 0;
	    MPI_Send(&status, 1, MPI_INT, 0, INFERNAL_READY_TAG, MPI_COMM_WORLD);
	    
	    /* wait for the next block of sequences */
	    MPI_Recv(&block, 3, MPI_LONG_LONG_INT, 0, INFERNAL_BLOCK_TAG, MPI_COMM_WORLD, &mpistatus);
	  }
	cm_file_Close(cmfp);
      } /* end loop over in_rc (reverse complement) */
      esl_stopwatch_Stop(w);
      
      /* Send the top hits back to the master. */
      cm_tophits_MPISend(th, 0, INFERNAL_TOPHITS_TAG, MPI_COMM_WORLD,  &mpi_buf, &mpi_size);
      cm_pipeline_MPISend(pli, 0, INFERNAL_PIPELINE_TAG, MPI_COMM_WORLD,  &mpi_buf, &mpi_size);
      
      cm_pipeline_Destroy(pli, NULL);
      cm_tophits_Destroy(th);
      esl_sq_Reuse(qsq);
    } /* end outer loop over query sequences */
  if (sstatus == eslEFORMAT) 
    mpi_failure("Parse failed (sequence file %s):\n%s\n", sqfp->filename, esl_sqfile_GetErrorBuf(sqfp));
  else if (sstatus != eslEOF)
    mpi_failure("Unexpected error %d reading sequence file %s", sstatus, sqfp->filename);
  
  status = 0;
  MPI_Send(&status, 1, MPI_INT, 0, INFERNAL_TERMINATING_TAG, MPI_COMM_WORLD);

  if (mpi_buf != NULL) free(mpi_buf);

  p7_bg_Destroy(bg);

  esl_sq_Destroy(qsq);
  esl_stopwatch_Destroy(w);
  esl_alphabet_Destroy(abc);
  esl_sqfile_Close(sqfp);

  return eslOK;

 ERROR: 
  mpi_failure("out of memory");
  return status; /* NEVER REACHED */
}
#endif /*HAVE_MPI*/

/* process_commandline()
 * 
 * Processes the commandline, filling in fields in <cfg> and creating and returning
 * an <ESL_GETOPTS> options structure. The help page (cmscan -h) is formatted
 * here.
 */
static void
process_commandline(int argc, char **argv, ESL_GETOPTS **ret_go, char **ret_cmfile, char **ret_seqfile)
{
  ESL_GETOPTS *go = NULL;

  if ((go = esl_getopts_Create(options))     == NULL)     cm_Fail("Internal failure creating options object");
  if (esl_opt_ProcessEnvironment(go)         != eslOK)  { printf("Failed to process environment: %s\n", go->errbuf); goto ERROR; }
  if (esl_opt_ProcessCmdline(go, argc, argv) != eslOK)  { printf("Failed to parse command line: %s\n",  go->errbuf); goto ERROR; }
  if (esl_opt_VerifyConfig(go)               != eslOK)  { printf("Failed to parse command line: %s\n",  go->errbuf); goto ERROR; }
 
  /* help format: (identical and synchronized with cmsearch.c:process_commandline()) */
  if (esl_opt_GetBoolean(go, "-h") || esl_opt_GetBoolean(go, "--devhelp")) {
    cm_banner(stdout, argv[0], banner);
    esl_usage(stdout, argv[0], usage);
    puts("\nBasic options:");
    esl_opt_DisplayHelp(stdout, go, 1, 2, 80); /* 1= group; 2 = indentation; 120=textwidth*/
    puts("\nOptions directing output:");
    esl_opt_DisplayHelp(stdout, go, 2, 2, 80); 
    puts("\nOptions controlling reporting thresholds:");
    esl_opt_DisplayHelp(stdout, go, 3, 2, 80); 
    puts("\nOptions controlling inclusion (significance) thresholds:");
    esl_opt_DisplayHelp(stdout, go, 4, 2, 80); 
    puts("\nOptions controlling model-specific reporting thresholds:");
    esl_opt_DisplayHelp(stdout, go, 5, 2, 80); 
    puts("\nOptions controlling acceleration heuristics:");
    esl_opt_DisplayHelp(stdout, go, 6, 2, 100);
    puts("\nOptions controlling detection of truncated hits:");
    esl_opt_DisplayHelp(stdout, go, 7, 2, 80); 
    puts("\nOther expert options:");
    esl_opt_DisplayHelp(stdout, go, 8, 2, 80); 
    if(esl_opt_GetBoolean(go, "--devhelp")) { 
      puts("\nDeveloper options controlling the filter pipeline:");
      esl_opt_DisplayHelp(stdout, go, 101, 2, 80);
      puts("\nDeveloper options controlling the CYK filter stage:");
      esl_opt_DisplayHelp(stdout, go, 102, 2, 80);
      puts("\nDeveloper options controlling the final Inside stage:");
      esl_opt_DisplayHelp(stdout, go, 103, 2, 80);
      puts("\nDeveloper options for timing pipeline stages:");
      esl_opt_DisplayHelp(stdout, go, 104, 2, 80);
      puts("\nDeveloper options controlling envelope definition:");
      esl_opt_DisplayHelp(stdout, go, 105, 2, 80);
      puts("\nOther developer options:");
      esl_opt_DisplayHelp(stdout, go, 106, 2, 80);
    }
    exit(0);
  }

  if (esl_opt_ArgNumber(go)                 != 2)      { puts("Incorrect number of command line arguments.");      goto ERROR; }
  if ((*ret_cmfile  = esl_opt_GetArg(go, 1)) == NULL)  { puts("Failed to get <cmdb> argument on command line");   goto ERROR; }
  if ((*ret_seqfile = esl_opt_GetArg(go, 2)) == NULL)  { puts("Failed to get <seqfile> argument on command line"); goto ERROR; }

  /* Check for incompatible option combinations I don't know how to disallow with esl_getopts */
  if (esl_opt_GetBoolean(go, "--qdb") && esl_opt_GetBoolean(go, "--fqdb")) { 
    if((esl_opt_GetReal(go, "--beta") - esl_opt_GetReal(go, "--fbeta")) > 1E-20) { 
      puts("Error parsing options, with --fbeta <x1> --beta <x2>, <x1> must be >= <x2>\n");
      exit(1);
    }
  }
  else if (esl_opt_GetBoolean(go, "--qdb")) { 
    if((esl_opt_GetReal(go, "--beta") - esl_opt_GetReal(go, "--fbeta")) > 1E-20) { 
      printf("Error parsing options, with --beta <x> (not in combination with --fbeta), <x> must be <= %g\n", esl_opt_GetReal(go, "--fbeta"));
      exit(1);
    }
  }
  else if (esl_opt_GetBoolean(go, "--fqdb")) { 
    if((esl_opt_GetReal(go, "--beta") - esl_opt_GetReal(go, "--fbeta")) > 1E-20) { 
      printf("Error parsing options, with --fbeta <x> (not in combination with --beta), <x> must be >= %g\n", esl_opt_GetReal(go, "--beta"));
      exit(1);
    }
  }

  /* Validate any attempted use of stdin streams */
  if (strcmp(*ret_cmfile, "-") == 0) {
    puts("cmscan cannot read <cm database> from stdin stream, because it must have cmpress'ed auxfiles");
    goto ERROR;
  }

  *ret_go = go;
  return;
  
 ERROR:  /* all errors handled here are user errors, so be polite.  */
  esl_usage(stdout, argv[0], usage);
  puts("\nwhere most common options are:");
  esl_opt_DisplayHelp(stdout, go, 1, 2, 80); /* 1= group; 2 = indentation; 80=textwidth*/
  printf("\nTo see more help on available options, do %s -h\n\n", argv[0]);
  exit(1);  
}

static int
output_header(FILE *ofp, const ESL_GETOPTS *go, char *cmfile, char *seqfile)
{
  cm_banner(ofp, go->argv[0], banner);
  
  fprintf(ofp, "# query sequence file:             %s\n", seqfile);
  fprintf(ofp, "# target CM database:              %s\n", cmfile);
  if (esl_opt_IsUsed(go, "-g"))           fprintf(ofp, "# CM configuration:                      glocal\n");
  if (esl_opt_IsUsed(go, "-Z"))           fprintf(ofp, "# database size is set to:               %.1f Mb\n",    esl_opt_GetReal(go, "-Z"));
  if (esl_opt_IsUsed(go, "-o"))           fprintf(ofp, "# output directed to file:               %s\n",      esl_opt_GetString(go, "-o"));
  if (esl_opt_IsUsed(go, "--tblout"))     fprintf(ofp, "# tabular output of hits:                %s\n",      esl_opt_GetString(go, "--tblout"));
  if (esl_opt_IsUsed(go, "--acc"))        fprintf(ofp, "# prefer accessions over names:          yes\n");
  if (esl_opt_IsUsed(go, "--noali"))      fprintf(ofp, "# show alignments in output:             no\n");
  if (esl_opt_IsUsed(go, "--notextw"))    fprintf(ofp, "# max ASCII text line length:            unlimited\n");
  if (esl_opt_IsUsed(go, "--textw"))      fprintf(ofp, "# max ASCII text line length:            %d\n",             esl_opt_GetInteger(go, "--textw"));
  if (esl_opt_IsUsed(go, "--allstats"))   fprintf(ofp, "# verbose pipeline statistics mode:      on\n");
  if (esl_opt_IsUsed(go, "-E"))           fprintf(ofp, "# sequence reporting threshold:          E-value <= %g\n",  esl_opt_GetReal(go, "-E"));
  if (esl_opt_IsUsed(go, "-T"))           fprintf(ofp, "# sequence reporting threshold:          score >= %g\n",    esl_opt_GetReal(go, "-T"));
  if (esl_opt_IsUsed(go, "--incE"))       fprintf(ofp, "# sequence inclusion threshold:          E-value <= %g\n",  esl_opt_GetReal(go, "--incE"));
  if (esl_opt_IsUsed(go, "--incT"))       fprintf(ofp, "# sequence inclusion threshold:          score >= %g\n",    esl_opt_GetReal(go, "--incT"));
  if (esl_opt_IsUsed(go, "--cut_ga"))     fprintf(ofp, "# model-specific thresholding:           GA cutoffs\n");
  if (esl_opt_IsUsed(go, "--cut_nc"))     fprintf(ofp, "# model-specific thresholding:           NC cutoffs\n");
  if (esl_opt_IsUsed(go, "--cut_tc"))     fprintf(ofp, "# model-specific thresholding:           TC cutoffs\n");
  if (esl_opt_IsUsed(go, "--max"))        fprintf(ofp, "# Max sensitivity mode:                  on [all heuristic filters off]\n");
  if (esl_opt_IsUsed(go, "--nohmm"))      fprintf(ofp, "# CM-only mode:                          on [HMM filters off]\n");
  if (esl_opt_IsUsed(go, "--mid"))        fprintf(ofp, "# HMM MSV and Viterbi filters:           off\n");
  if (esl_opt_IsUsed(go, "--rfam"))       fprintf(ofp, "# Rfam pipeline mode:                    on [strict filtering]\n");
  //  if (esl_opt_IsUsed(go, "--hmm"))        fprintf(ofp, "# HMM-only mode:                         on [ignoring structure]\n");
  if (esl_opt_IsUsed(go, "--FZ"))         fprintf(ofp, "# Filters set as if DB size in Mb is:    %f\n", esl_opt_GetReal(go, "--FZ"));
  if (esl_opt_IsUsed(go, "--Fmid"))       fprintf(ofp, "# HMM Forward filter thresholds set to:  %g\n", esl_opt_GetReal(go, "--Fmid"));
  if (esl_opt_IsUsed(go, "--notrunc"))    fprintf(ofp, "# truncated sequence detection:          off\n");
  if (esl_opt_IsUsed(go, "--anytrunc"))   fprintf(ofp, "# allowing truncated sequences anywhere: on\n");
  if (esl_opt_IsUsed(go, "--nonull3"))    fprintf(ofp, "# null3 bias corrections:                off\n");
  if (esl_opt_IsUsed(go, "--mxsize"))     fprintf(ofp, "# maximum DP alignment matrix size:      %.1f Mb\n", esl_opt_GetReal(go, "--mxsize"));
  if (esl_opt_IsUsed(go, "--cyk"))        fprintf(ofp, "# use CYK for final search stage         on\n");
  if (esl_opt_IsUsed(go, "--acyk"))       fprintf(ofp, "# use CYK to align hits:                 on\n");
  if (esl_opt_IsUsed(go, "--toponly"))    fprintf(ofp, "# search top-strand only:                on\n");
  if (esl_opt_IsUsed(go, "--bottomonly")) fprintf(ofp, "# search bottom-strand only:             on\n");
  if (esl_opt_IsUsed(go, "--qformat"))    fprintf(ofp, "# query <seqfile> format asserted:       %s\n", esl_opt_GetString(go, "--qformat"));
#ifdef HMMER_THREADS
  if (esl_opt_IsUsed(go, "--cpu"))       fprintf(ofp, "# number of worker threads:               %d\n", esl_opt_GetInteger(go, "--cpu"));  
#endif
#ifdef HAVE_MPI
  if (esl_opt_IsUsed(go, "--stall"))     fprintf(ofp, "# MPI stall mode:                        on\n");
  if (esl_opt_IsUsed(go, "--mpi"))       fprintf(ofp, "# MPI:                                   on\n");
#endif
  /* Developer options, only shown to user if --devhelp used */
  if (esl_opt_IsUsed(go, "--noF1"))       fprintf(ofp, "# HMM MSV filter:                        off\n");
  if (esl_opt_IsUsed(go, "--noF2"))       fprintf(ofp, "# HMM Vit filter:                        off\n");
  if (esl_opt_IsUsed(go, "--noF3"))       fprintf(ofp, "# HMM Fwd filter:                        off\n");
  if (esl_opt_IsUsed(go, "--noF4"))       fprintf(ofp, "# HMM glocal Fwd filter:                 off\n");
  if (esl_opt_IsUsed(go, "--noF6"))       fprintf(ofp, "# CM CYK filter:                         off\n");
  if (esl_opt_IsUsed(go, "--doF1b"))      fprintf(ofp, "# HMM MSV biased comp filter:            on\n");
  if (esl_opt_IsUsed(go, "--noF2b"))      fprintf(ofp, "# HMM Vit biased comp filter:            off\n");
  if (esl_opt_IsUsed(go, "--noF3b"))      fprintf(ofp, "# HMM Fwd biased comp filter:            off\n");
  if (esl_opt_IsUsed(go, "--noF4b"))      fprintf(ofp, "# HMM gFwd biased comp filter:           off\n");
  if (esl_opt_IsUsed(go, "--noF5b"))      fprintf(ofp, "# HMM per-envelope biased comp filter:   off\n");
  if (esl_opt_IsUsed(go, "--F1"))         fprintf(ofp, "# HMM MSV filter P threshold:            <= %g\n", esl_opt_GetReal(go, "--F1"));
  if (esl_opt_IsUsed(go, "--F1b"))        fprintf(ofp, "# HMM MSV bias P threshold:              <= %g\n", esl_opt_GetReal(go, "--F1b"));
  if (esl_opt_IsUsed(go, "--F2"))         fprintf(ofp, "# HMM Vit filter P threshold:            <= %g\n", esl_opt_GetReal(go, "--F2"));
  if (esl_opt_IsUsed(go, "--F2b"))        fprintf(ofp, "# HMM Vit bias P threshold:              <= %g\n", esl_opt_GetReal(go, "--F2b"));
  if (esl_opt_IsUsed(go, "--F3"))         fprintf(ofp, "# HMM Fwd filter P threshold:            <= %g\n", esl_opt_GetReal(go, "--F3"));
  if (esl_opt_IsUsed(go, "--F3b"))        fprintf(ofp, "# HMM Fwd bias P threshold:              <= %g\n", esl_opt_GetReal(go, "--F3b"));
  if (esl_opt_IsUsed(go, "--F4"))         fprintf(ofp, "# HMM glocal Fwd filter P threshold:     <= %g\n", esl_opt_GetReal(go, "--F4"));
  if (esl_opt_IsUsed(go, "--F4b"))        fprintf(ofp, "# HMM glocal Fwd bias P threshold:       <= %g\n", esl_opt_GetReal(go, "--F4b"));
  if (esl_opt_IsUsed(go, "--F5"))         fprintf(ofp, "# HMM env defn filter P threshold:       <= %g\n", esl_opt_GetReal(go, "--F5"));
  if (esl_opt_IsUsed(go, "--F5b"))        fprintf(ofp, "# HMM env defn bias   P threshold:       <= %g\n", esl_opt_GetReal(go, "--F5b"));
  if (esl_opt_IsUsed(go, "--F6"))         fprintf(ofp, "# CM CYK filter P threshold:             <= %g\n", esl_opt_GetReal(go, "--F6"));

  if (esl_opt_IsUsed(go, "--ftau"))       fprintf(ofp, "# tau parameter for CYK filter stage:    %g\n", esl_opt_GetReal(go, "--ftau"));
  if (esl_opt_IsUsed(go, "--fsums"))      fprintf(ofp, "# posterior sums (CYK filter stage):     on\n");
  if (esl_opt_IsUsed(go, "--fqdb"))       fprintf(ofp, "# QDBs (CYK filter stage)                on\n");
  if (esl_opt_IsUsed(go, "--fbeta"))      fprintf(ofp, "# beta parameter for CYK filter stage:   %g\n", esl_opt_GetReal(go, "--fbeta"));
  if (esl_opt_IsUsed(go, "--fnonbanded")) fprintf(ofp, "# no bands (CYK filter stage)            on\n");
  if (esl_opt_IsUsed(go, "--nocykenv"))   fprintf(ofp, "# CYK envelope redefinition:             off\n");
  if (esl_opt_IsUsed(go, "--cykenvx"))    fprintf(ofp, "# CYK envelope redefn P-val multiplier:  %g\n",  esl_opt_GetReal(go, "--cykenvx"));   
  if (esl_opt_IsUsed(go, "--tau"))        fprintf(ofp, "# tau parameter for final stage:         %g\n", esl_opt_GetReal(go, "--tau"));
  if (esl_opt_IsUsed(go, "--sums"))       fprintf(ofp, "# posterior sums (final stage):          on\n");
  if (esl_opt_IsUsed(go, "--qdb"))        fprintf(ofp, "# QDBs (final stage)                     on\n");
  if (esl_opt_IsUsed(go, "--beta"))       fprintf(ofp, "# beta parameter for final stage:        %g\n", esl_opt_GetReal(go, "--beta"));
  if (esl_opt_IsUsed(go, "--nonbanded"))  fprintf(ofp, "# no bands (final stage)                 on\n");
  if (esl_opt_IsUsed(go, "--time-F1"))    fprintf(ofp, "# abort after Stage 1 MSV (for timing)   on\n");
  if (esl_opt_IsUsed(go, "--time-F2"))    fprintf(ofp, "# abort after Stage 2 Vit (for timing)   on\n");
  if (esl_opt_IsUsed(go, "--time-F3"))    fprintf(ofp, "# abort after Stage 3 Fwd (for timing)   on\n");
  if (esl_opt_IsUsed(go, "--time-F4"))    fprintf(ofp, "# abort after Stage 4 gFwd (for timing)  on\n");
  if (esl_opt_IsUsed(go, "--time-F5"))    fprintf(ofp, "# abort after Stage 5 env defn (for timing) on\n");
  if (esl_opt_IsUsed(go, "--time-F6"))    fprintf(ofp, "# abort after Stage 6 CYK (for timing)   on\n");
  if (esl_opt_IsUsed(go, "--rt1"))        fprintf(ofp, "# domain definition rt1 parameter        %g\n", esl_opt_GetReal(go, "--rt1"));
  if (esl_opt_IsUsed(go, "--rt2"))        fprintf(ofp, "# domain definition rt2 parameter        %g\n", esl_opt_GetReal(go, "--rt2"));
  if (esl_opt_IsUsed(go, "--rt3"))        fprintf(ofp, "# domain definition rt3 parameter        %g\n", esl_opt_GetReal(go, "--rt3"));
  if (esl_opt_IsUsed(go, "--ns"))         fprintf(ofp, "# number of envelope tracebacks sampled  %d\n", esl_opt_GetInteger(go, "--ns"));

  if (esl_opt_IsUsed(go, "--anonbanded")) fprintf(ofp, "# no bands (hit alignment)               on\n");
  if (esl_opt_IsUsed(go, "--anewbands"))  fprintf(ofp, "# new bands (hit alignment)              on\n");
  if (esl_opt_IsUsed(go, "--envhitbias")) fprintf(ofp, "# envelope bias only for envelope:       on\n");
  if (esl_opt_IsUsed(go, "--nogreedy"))   fprintf(ofp, "# greedy CM hit resolution:              off\n");
  if (esl_opt_IsUsed(go, "--filcmW"))     fprintf(ofp, "# always use CM's W parameter:           on\n");
  if (esl_opt_IsUsed(go, "--cp9noel"))    fprintf(ofp, "# CP9 HMM local ends:                    off\n");
  if (esl_opt_IsUsed(go, "--cp9gloc"))    fprintf(ofp, "# CP9 HMM configuration:                 glocal\n");
  if (esl_opt_IsUsed(go, "--null2"))      fprintf(ofp, "# null2 bias corrections:                on\n");
  if (esl_opt_IsUsed(go, "--xtau"))       fprintf(ofp, "# tau multiplier for band tightening:    %g\n", esl_opt_GetReal(go, "--xtau"));
  if (esl_opt_IsUsed(go, "--seed"))  {
    if (esl_opt_GetInteger(go, "--seed") == 0) fprintf(ofp, "# random number seed:                    one-time arbitrary\n");
    else                                       fprintf(ofp, "# random number seed set to:             %d\n", esl_opt_GetInteger(go, "--seed"));
  }
  fprintf(ofp, "# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -\n\n");
  return eslOK;

}

#if HAVE_MPI 
/* mpi_failure()
 * Generate an error message.  If the clients rank is not 0, a
 * message is created with the error message and sent to the
 * master process for handling.
 */
static void
mpi_failure(char *format, ...)
{
  va_list  argp;
  int      status = eslFAIL;
  int      len;
  int      rank;
  char     str[512];

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  /* format the error mesg */
  va_start(argp, format);
  len = vsnprintf(str, sizeof(str), format, argp);
  va_end(argp);

  /* make sure the error string is terminated */
  str[sizeof(str)-1] = '\0';

  /* if the caller is the master, print the results and abort */
  if (rank == 0)
    {
      fprintf(stderr, "\nError: ");
      fprintf(stderr, "%s", str);
      fprintf(stderr, "\n");
      fflush(stderr);

      MPI_Abort(MPI_COMM_WORLD, status);
      exit(1);
    }
  else
    {
      MPI_Send(str, len, MPI_CHAR, 0, INFERNAL_ERROR_TAG, MPI_COMM_WORLD);
      pause();
    }
}

/* This routine parses the database keeping track of the blocks
 * offset within the file, number of sequences and the length
 * of the block.  These blocks are passed as work units to the
 * MPI workers.  If multiple hmm's are in the query file, the
 * blocks are reused without parsing the database a second time.
 */
int mpi_next_block(CM_FILE *cmfp, BLOCK_LIST *list, MSV_BLOCK *block)
{
  P7_OPROFILE   *om       = NULL;
  ESL_ALPHABET  *abc      = NULL;
  int            status   = eslOK;
  off_t          prv_offset = 0;

  /* if the list has been calculated, use it instead of parsing the database */
  if (list->complete)
    {
      if (list->current == list->last)
	{
	  block->offset = 0;
	  block->length = 0;
	  block->count  = 0;

	  status = eslEOF;
	}
      else
	{
	  int inx = list->current++;

	  block->offset = list->blocks[inx].offset;
	  block->length = list->blocks[inx].length;
	  block->count  = list->blocks[inx].count;

	  status = eslOK;
	}

      return status;
    }

  block->offset = 0;
  block->length = 0;
  block->count = 0;
  if((prv_offset = ftello(cmfp->ffp)) < 0) return eslESYS;

  while (block->length < MAX_BLOCK_SIZE && 
	 (status = cm_p7_oprofile_ReadMSV(cmfp, FALSE, &abc, NULL, NULL, NULL, NULL, NULL, &om)) == eslOK)
    {
      if (block->count == 0) block->offset = prv_offset;
      if((prv_offset = ftello(cmfp->ffp)) < 0) return eslESYS;
      block->length = om->eoff - block->offset + 1;
      block->count++;
      p7_oprofile_Destroy(om);
    }

  if (status == eslEOF && block->count > 0) status = eslOK;
  if (status == eslEOF) list->complete = 1;

  /* add the block to the list of known blocks */
  if (status == eslOK)
    {
      int inx;

      if (list->last >= list->size)
	{
	  void *tmp;
	  list->size += 500;
	  ESL_RALLOC(list->blocks, tmp, sizeof(MSV_BLOCK) * list->size);
	}

      inx = list->last++;
      list->blocks[inx].offset = block->offset;
      list->blocks[inx].length = block->length;
      list->blocks[inx].count  = block->count;
    }

  return status;

 ERROR:
  return eslEMEM;
}
#endif /* HAVE_MPI */

/*****************************************************************
 * @LICENSE@
 *****************************************************************/

