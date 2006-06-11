/**CFile****************************************************************

  FileName    [abcProve.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Network and node package.]

  Synopsis    [Proves the miter using AIG rewriting, FRAIGing, and SAT solving.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: abcProve.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#include "abc.h"
#include "fraig.h"
#include "math.h"

////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

extern int  Abc_NtkRewrite( Abc_Ntk_t * pNtk, int fUpdateLevel, int fUseZeros, int fVerbose );
extern int  Abc_NtkRefactor( Abc_Ntk_t * pNtk, int nNodeSizeMax, int nConeSizeMax, bool fUpdateLevel, bool fUseZeros, bool fUseDcs, bool fVerbose );
extern Abc_Ntk_t * Abc_NtkFromFraig( Fraig_Man_t * pMan, Abc_Ntk_t * pNtk );

static Abc_Ntk_t * Abc_NtkMiterFraig( Abc_Ntk_t * pNtk, int nBTLimit, sint64 nInspLimit, int * pRetValue, int * pNumFails, sint64 * pNumConfs, sint64 * pNumInspects );
static void Abc_NtkMiterPrint( Abc_Ntk_t * pNtk, char * pString, int clk, int fVerbose );


////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////
  
/**Function*************************************************************

  Synopsis    [Attempts to solve the miter using a number of tricks.]

  Description [Returns -1 if timed out; 0 if SAT; 1 if UNSAT. Returns
  a simplified version of the original network (or a constant 0 network).
  In case the network is not a constant zero and a SAT assignment is found,
  pNtk->pModel contains a satisfying assignment.]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Abc_NtkMiterProve( Abc_Ntk_t ** ppNtk, void * pPars )
{
    Prove_Params_t * pParams = pPars;
    Abc_Ntk_t * pNtk, * pNtkTemp;
    int RetValue, nIter, nSatFails, Counter, clk, timeStart = clock();
    sint64 nSatConfs, nSatInspects, nInspectLimit;

    // get the starting network
    pNtk = *ppNtk;
    assert( Abc_NtkIsStrash(pNtk) );
    assert( Abc_NtkPoNum(pNtk) == 1 );
 
    if ( pParams->fVerbose )
    {
        printf( "RESOURCE LIMITS: Iterations = %d. Rewriting = %s. Fraiging = %s.\n",
            pParams->nItersMax, pParams->fUseRewriting? "yes":"no", pParams->fUseFraiging? "yes":"no" );
        printf( "Mitering = %d (%3.1f).  Rewriting = %d (%3.1f).  Fraiging = %d (%3.1f).\n", 
            pParams->nMiteringLimitStart,  pParams->nMiteringLimitMulti, 
            pParams->nRewritingLimitStart, pParams->nRewritingLimitMulti,
            pParams->nFraigingLimitStart,  pParams->nFraigingLimitMulti );
        printf( "Mitering last = %d.\n", 
            pParams->nMiteringLimitLast );
    }

    // if SAT only, solve without iteration
    if ( !pParams->fUseRewriting && !pParams->fUseFraiging )
    {
        clk = clock();
        RetValue = Abc_NtkMiterSat( pNtk, (sint64)pParams->nMiteringLimitLast, (sint64)0, 0, 0, NULL, NULL );
        Abc_NtkMiterPrint( pNtk, "SAT solving", clk, pParams->fVerbose );
        *ppNtk = pNtk;
        return RetValue;
    }

    // check the current resource limits
    for ( nIter = 0; nIter < pParams->nItersMax; nIter++ )
    {
        if ( pParams->fVerbose )
        {
            printf( "ITERATION %2d : Confs = %6d. FraigBTL = %3d. \n", nIter+1, 
                 (int)(pParams->nMiteringLimitStart * pow(pParams->nMiteringLimitMulti,nIter)), 
                 (int)(pParams->nFraigingLimitStart * pow(pParams->nFraigingLimitMulti,nIter)) );
            fflush( stdout );
        }

        // try brute-force SAT
        clk = clock();
        nInspectLimit = pParams->nTotalInspectLimit? pParams->nTotalInspectLimit - pParams->nTotalInspectsMade : 0;
        RetValue = Abc_NtkMiterSat( pNtk, (sint64)(pParams->nMiteringLimitStart * pow(pParams->nMiteringLimitMulti,nIter)), (sint64)nInspectLimit, 0, 0, &nSatConfs, &nSatInspects );
        Abc_NtkMiterPrint( pNtk, "SAT solving", clk, pParams->fVerbose );
        if ( RetValue >= 0 )
            break;

        // add to the number of backtracks and inspects
        pParams->nTotalBacktracksMade += nSatConfs;
        pParams->nTotalInspectsMade   += nSatInspects;
        // check if global resource limit is reached
        if ( (pParams->nTotalBacktrackLimit && pParams->nTotalBacktracksMade >= pParams->nTotalBacktrackLimit) ||
             (pParams->nTotalInspectLimit   && pParams->nTotalInspectsMade   >= pParams->nTotalInspectLimit) )
        {
            printf( "Reached global limit on conflicts/inspects. Quitting.\n" );
            *ppNtk = pNtk;
            return -1;
        }

        // try rewriting
        if ( pParams->fUseRewriting )
        {
            clk = clock();
            Counter = (int)(pParams->nRewritingLimitStart * pow(pParams->nRewritingLimitMulti,nIter));
            while ( 1 )
            {
                Abc_NtkRewrite( pNtk, 0, 0, 0 );
                if ( (RetValue = Abc_NtkMiterIsConstant(pNtk)) >= 0 )
                    break;
                if ( --Counter == 0 )
                    break;
                Abc_NtkRefactor( pNtk, 10, 16, 0, 0, 0, 0 );
                if ( (RetValue = Abc_NtkMiterIsConstant(pNtk)) >= 0 )
                    break;
                if ( --Counter == 0 )
                    break;
                pNtk = Abc_NtkBalance( pNtkTemp = pNtk, 0, 0, 0 );  Abc_NtkDelete( pNtkTemp );
                if ( (RetValue = Abc_NtkMiterIsConstant(pNtk)) >= 0 )
                    break;
                if ( --Counter == 0 )
                    break;
            }
            Abc_NtkMiterPrint( pNtk, "Rewriting  ", clk, pParams->fVerbose );
        }
 
        if ( pParams->fUseFraiging )
        {
            // try FRAIGing
            clk = clock();
            nInspectLimit = pParams->nTotalInspectLimit? pParams->nTotalInspectLimit - pParams->nTotalInspectsMade : 0;
            pNtk = Abc_NtkMiterFraig( pNtkTemp = pNtk, (int)(pParams->nFraigingLimitStart * pow(pParams->nFraigingLimitMulti,nIter)), nInspectLimit, &RetValue, &nSatFails, &nSatConfs, &nSatInspects );  Abc_NtkDelete( pNtkTemp );
            Abc_NtkMiterPrint( pNtk, "FRAIGing   ", clk, pParams->fVerbose );
//            printf( "NumFails = %d\n", nSatFails );
            if ( RetValue >= 0 )
                break;

            // add to the number of backtracks and inspects
            pParams->nTotalBacktracksMade += nSatConfs;
            pParams->nTotalInspectsMade += nSatInspects;
            // check if global resource limit is reached
            if ( (pParams->nTotalBacktrackLimit && pParams->nTotalBacktracksMade >= pParams->nTotalBacktrackLimit) ||
                 (pParams->nTotalInspectLimit   && pParams->nTotalInspectsMade   >= pParams->nTotalInspectLimit) )
            {
                printf( "Reached global limit on conflicts/inspects. Quitting.\n" );
                *ppNtk = pNtk;
                return -1;
            }
        }

    }    

    // try to prove it using brute force SAT
    if ( RetValue < 0 && pParams->fUseBdds )
    {
        if ( pParams->fVerbose )
        {
            printf( "Attempting BDDs with node limit %d ...\n", pParams->nBddSizeLimit );
            fflush( stdout );
        }
        clk = clock();
        pNtk = Abc_NtkCollapse( pNtkTemp = pNtk, pParams->nBddSizeLimit, 0, pParams->fBddReorder, 0 );
        if ( pNtk )   
        {
            Abc_NtkDelete( pNtkTemp );
            RetValue = ( (Abc_NtkNodeNum(pNtk) == 1) && (Abc_ObjFanin0(Abc_NtkPo(pNtk,0))->pData == Cudd_ReadLogicZero(pNtk->pManFunc)) );
        }
        else 
            pNtk = pNtkTemp;
        Abc_NtkMiterPrint( pNtk, "BDD building", clk, pParams->fVerbose );
    }

    if ( RetValue < 0 )
    {
        if ( pParams->fVerbose )
        {
            printf( "Attempting SAT with conflict limit %d ...\n", pParams->nMiteringLimitLast );
            fflush( stdout );
        }
        clk = clock();
        nInspectLimit = pParams->nTotalInspectLimit? pParams->nTotalInspectLimit - pParams->nTotalInspectsMade : 0;
        RetValue = Abc_NtkMiterSat( pNtk, (sint64)pParams->nMiteringLimitLast, (sint64)nInspectLimit, 0, 0, NULL, NULL );
        Abc_NtkMiterPrint( pNtk, "SAT solving", clk, pParams->fVerbose );
    }

    // assign the model if it was proved by rewriting (const 1 miter)
    if ( RetValue == 0 && pNtk->pModel == NULL )
    {
        pNtk->pModel = ALLOC( int, Abc_NtkCiNum(pNtk) );
        memset( pNtk->pModel, 0, sizeof(int) * Abc_NtkCiNum(pNtk) );
    }
    *ppNtk = pNtk;
    return RetValue;
}

/**Function*************************************************************

  Synopsis    [Attempts to solve the miter using a number of tricks.]

  Description [Returns -1 if timed out; 0 if SAT; 1 if UNSAT.]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Abc_Ntk_t * Abc_NtkMiterFraig( Abc_Ntk_t * pNtk, int nBTLimit, sint64 nInspLimit, int * pRetValue, int * pNumFails, sint64 * pNumConfs, sint64 * pNumInspects )
{
    Abc_Ntk_t * pNtkNew;
    Fraig_Params_t Params, * pParams = &Params;
    Fraig_Man_t * pMan;
    int nWords1, nWords2, nWordsMin, RetValue;
    int * pModel;

    // to determine the number of simulation patterns
    // use the following strategy
    // at least 64 words (32 words random and 32 words dynamic)
    // no more than 256M for one circuit (128M + 128M)
    nWords1 = 32;
    nWords2 = (1<<27) / (Abc_NtkNodeNum(pNtk) + Abc_NtkCiNum(pNtk));
    nWordsMin = ABC_MIN( nWords1, nWords2 );

    // set the FRAIGing parameters
    Fraig_ParamsSetDefault( pParams );
    pParams->nPatsRand  = nWordsMin * 32; // the number of words of random simulation info
    pParams->nPatsDyna  = nWordsMin * 32; // the number of words of dynamic simulation info
    pParams->nBTLimit   = nBTLimit;       // the max number of backtracks
    pParams->nSeconds   = -1;             // the runtime limit
    pParams->fTryProve  = 0;              // do not try to prove the final miter
    pParams->fDoSparse  = 1;              // try proving sparse functions
    pParams->fVerbose   = 0;
    pParams->nInspLimit = nInspLimit;

    // transform the target into a fraig
    pMan = Abc_NtkToFraig( pNtk, pParams, 0, 0 ); 
    Fraig_ManProveMiter( pMan );
    RetValue = Fraig_ManCheckMiter( pMan );

    // create the network 
    pNtkNew = Abc_NtkFromFraig( pMan, pNtk );

    // save model
    if ( RetValue == 0 )
    {
        pModel = Fraig_ManReadModel( pMan );
        FREE( pNtkNew->pModel );
        pNtkNew->pModel = ALLOC( int, Abc_NtkCiNum(pNtkNew) );
        memcpy( pNtkNew->pModel, pModel, sizeof(int) * Abc_NtkCiNum(pNtkNew) );
    }

    // save the return values
    *pRetValue = RetValue;
    *pNumFails = Fraig_ManReadSatFails( pMan );
    *pNumConfs = Fraig_ManReadConflicts( pMan );
    *pNumInspects = Fraig_ManReadInspects( pMan );

    // delete the fraig manager
    Fraig_ManFree( pMan );
    return pNtkNew;
}

/**Function*************************************************************

  Synopsis    [Attempts to solve the miter using a number of tricks.]

  Description [Returns -1 if timed out; 0 if SAT; 1 if UNSAT.]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Abc_NtkMiterPrint( Abc_Ntk_t * pNtk, char * pString, int clk, int fVerbose )
{
    if ( !fVerbose )
        return;
    printf( "Nodes = %7d.  Levels = %4d.  ", Abc_NtkNodeNum(pNtk), 
        Abc_NtkIsStrash(pNtk)? Abc_AigGetLevelNum(pNtk) : Abc_NtkGetLevelNum(pNtk) );
    PRT( pString, clock() - clk );
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


