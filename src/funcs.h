#include "squid.h"
#include "msa.h"
#include "structs.h"

/* from modelmaker.c
 */
extern void HandModelmaker(MSA *msa, int use_rf, float gapthresh, CM_t **ret_cm, Parsetree_t **ret_mtr);

/* from parsetree.c
 */
extern Parsetree_t *CreateParsetree(void);
extern void         GrowParsetree(Parsetree_t *tr);
extern void         FreeParsetree(Parsetree_t *tr);
extern int          InsertTraceNode(Parsetree_t *tr, int y, int whichway, 
				    int emitl, int emitr, int state, int type);
extern void         PrintParsetree(FILE *fp, Parsetree_t *tr);
extern void         SummarizeMasterTrace(FILE *fp, Parsetree_t *tr);

/* from rna_ops.c
 */
extern int KHS2ct(char *ss, int len, int allow_pseudoknots, int **ret_ct);
