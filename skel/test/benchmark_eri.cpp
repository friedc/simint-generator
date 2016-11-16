#include <cstdio>

#include "simint/simint.h"
#include "test/Common.hpp"
#include "test/Timer.h"

#include <omp.h>

#ifdef BENCHMARK_VALIDATE
#include "test/ValeevRef.hpp"
#endif

#define SIMINT_SCREEN 0
#define SIMINT_SCREEN_TOL 0.0

int main(int argc, char ** argv)
{
    // set up the function pointers
    simint_init();

    if(argc != 2)
    {
        printf("Give me 1 argument! I got %d\n", argc-1);
        return 1;
    }

    // files to read
    std::string basfile(argv[1]);

    // number of threads
    const int nthread = omp_get_max_threads();

    // read in the shell info
    ShellMap shellmap = ReadBasis(basfile);

    // normalize
    for(auto & it : shellmap)
        simint_normalize_shells(it.second.size(), it.second.data());


    // find the max dimensions
    std::pair<int, int> maxparams = FindMaxParams(shellmap);
    const int maxam = (maxparams.first > SIMINT_OSTEI_MAXAM ? SIMINT_OSTEI_MAXAM : maxparams.first);
    const int max_ncart = ( (maxam+1)*(maxam+2) )/2;
    const int maxsize = maxparams.second * maxparams.second * max_ncart * max_ncart;

    /* Storage of integrals */
    double * all_res_ints = (double *)SIMINT_ALLOC(nthread * maxsize * sizeof(double));

    /* contracted workspace */
    double * all_simint_work = (double *)SIMINT_ALLOC(nthread * SIMINT_OSTEI_MAX_WORKMEM);


    // initialize stuff
    // nothing needs initializing!

    #ifdef BENCHMARK_VALIDATE
    ValeevRef_Init();
    double * all_res_ref = (double *)SIMINT_ALLOC(nthread * maxsize * sizeof(double));
    #endif

    PrintTimingHeader();

    // running totals
    size_t ncont1234_total = 0;
    size_t nprim1234_total = 0;
    size_t nshell1234_total = 0;
    size_t skipped_total = 0;
    TimeContrib time_total;


    for(int i = 0; i <= maxam; i++)
    for(int j = 0; j <= maxam; j++)
    for(int k = 0; k <= maxam; k++)
    for(int l = 0; l <= maxam; l++)
    {
        if(!UniqueQuartet(i, j, k, l))
            continue;

        #ifdef BENCHMARK_VALIDATE
        std::pair<double, double> err(0.0, 0.0);
        #endif

        const size_t ncart1234 = NCART(i) * NCART(j) * NCART(k) * NCART(l);

        // total amount of time for this AM quartet
        TimeContrib time_am;

        const size_t nshell3 = shellmap[k].size();
        const size_t nshell4 = shellmap[l].size();

        simint_shell const * const C = &shellmap[k][0];
        simint_shell const * const D = &shellmap[l][0];

        // time creation of Q
        TimerType time_pair_34_0, time_pair_34_1;
        CLOCK(time_pair_34_0);


        struct simint_multi_shellpair Q;
        simint_initialize_multi_shellpair(&Q);
        simint_create_multi_shellpair(nshell3, C, nshell4, D, &Q, SIMINT_SCREEN);
        CLOCK(time_pair_34_1);
        time_am.fill_shell_pair += time_pair_34_1 - time_pair_34_0;


        // running totals for this am
        std::atomic<size_t> nprim1234_am(0);
        std::atomic<size_t> nshell1234_am(0);
        std::atomic<size_t> ncont1234_am(0);

	    const auto & shellmap_i = shellmap[i];
	    const auto & shellmap_j = shellmap[j];

        const size_t i_size = shellmap_i.size();
        const size_t j_size = shellmap_j.size();
	    const size_t brasize = i_size * j_size;

        // do one shell pair at a time on the bra side
        #pragma omp parallel for schedule(dynamic)
        for(size_t ab = 0; ab < brasize; ab++)
        {

	        size_t a = ab / j_size;
    	    size_t b = ab % j_size;

            const size_t nshell1 = 1;
            const size_t nshell2 = 1;

            simint_shell const * const A = &shellmap_i[a];
            simint_shell const * const B = &shellmap_j[b];

            // time creation of P
            TimerType time_pair_12_0, time_pair_12_1;
            CLOCK(time_pair_12_0);
            struct simint_multi_shellpair P;
            simint_initialize_multi_shellpair(&P);
            simint_create_multi_shellpair(nshell1, A, nshell2, B, &P, SIMINT_SCREEN);
            CLOCK(time_pair_12_1);

            // acutal number of primitives and shells that
            // will be calculated
            const size_t nprim1234 = P.nprim * Q.nprim;
            const size_t nshell1234 = P.nshell12 * Q.nshell12;
            const size_t ncont1234 = nshell1234 * ncart1234;

            int ithread = omp_get_thread_num();
            double * res_ints = all_res_ints + ithread * maxsize;
            double * const simint_work = all_simint_work + ithread * SIMINT_OSTEI_MAX_WORKSIZE;


            // actually calculate
            TimerType simint_ticks_0, simint_ticks_1;
            CLOCK(simint_ticks_0);
            int simint_ret = simint_compute_eri_sharedwork(&P, &Q, SIMINT_SCREEN_TOL, simint_work, res_ints);
            CLOCK(simint_ticks_1);

            // if the return is < 0, it didn't calculate anything
            // (everything was screened)
            if(simint_ret < 0)
                std::fill(res_ints, res_ints + ncont1234, 0.0);


            #ifdef BENCHMARK_VALIDATE
            double * res_ref = all_res_ref + ithread * maxsize;

            ValeevRef_Integrals(A, nshell1,
                                B, nshell2,
                                C, nshell3,
                                D, nshell4,
                                res_ref, false);
            std::pair<double, double> err2 = CalcError(res_ints, res_ref, ncont1234);

            #pragma omp critical
            {
                err.first = std::max(err.first, err2.first);
                err.second = std::max(err.second, err2.second);
                if(simint_ret < 0)
                    skipped_total += ncont1234;            
            }
            #endif // closes BENCHMARK_VALIDATE

            // free this here, since we are done with it
            simint_free_multi_shellpair(&P);


            // add primitive and shell count to running totals for this am
            ncont1234_am += ncont1234;
            nprim1234_am += nprim1234;
            nshell1234_am += nshell1234;
            time_am.integrals += (simint_ticks_1 - simint_ticks_0);
            time_am.fill_shell_pair += time_pair_12_1 - time_pair_12_0;
        }

        simint_free_multi_shellpair(&Q);

		// add primitive and shell count to overall running totals
		// threadsafe since these are std::atomic
		ncont1234_total += ncont1234_am;
		nprim1234_total += nprim1234_am;
		nshell1234_total += nshell1234_am;
		time_total += time_am;

        PrintAMTimingInfo(i, j, k, l, nshell1234_am, nprim1234_am, time_am);


        #ifdef BENCHMARK_VALIDATE
        printf("( %d %d | %d %d ) MaxAbsErr: %10.3e   MaxRelErr: %10.3e\n", i, j, k, l, err.first, err.second);
        #endif
    }

    printf("\n");
    printf("Calculated %lu contracted integrals\n", static_cast<unsigned long>(ncont1234_total));
    printf("Calculated %lu contracted shells\n", static_cast<unsigned long>(nshell1234_total));
    printf("Calculated %lu primitive integrals\n", static_cast<unsigned long>(nprim1234_total));
    printf("Total ticks to calculate all:  %llu\n", time_total.TotalTime());
    printf("Skipped %lu top-level contracted shells\n", static_cast<unsigned long>(skipped_total));

    printf("\n");

    FreeShellMap(shellmap);

    SIMINT_FREE(all_res_ints);
    SIMINT_FREE(all_simint_work);


    #ifdef BENCHMARK_VALIDATE
    SIMINT_FREE(all_res_ref);
    ValeevRef_Finalize();
    #endif
    
    // Finalize stuff 
    simint_finalize();

    return 0;
}
