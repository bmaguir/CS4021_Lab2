#include "stdafx.h"                             // pre-compiled headers
#include <iostream>                             // cout
#include <iomanip>                              // setprecision
#include "helper.h"                             //

using namespace std;                            // cout

#define K           1024                        //
#define GB          (K*K*K)                     //
#define NOPS        10                        //
#define NSECONDS    5                           // run each test for NSECONDS

UINT64 tstart;                                  // start of test in ms
int sharing;                                    // % sharing
int lineSz;                                     // cache line size
int maxThread;                                  // max # of threads

THREADH *threadH;                               // thread handles
UINT64 *ops;                                    // for ops per thread

#define VINT    UINT64                          //  64 bit counter
#define ALIGNED_MALLOC(sz, align) _aligned_malloc(sz, align)
#define GINDX(n)    (g+n*lineSz/sizeof(VINT))   //

volatile UINT64 share_inc;

typedef struct {
    int sharing;                                // sharing
    int nt;                                     // # threads
    UINT64 rt;                                  // run time (ms)
    UINT64 ops;                                 // ops
    UINT64 incs;                                // should be equal ops
    UINT64 aborts;                              //
} Result;

Result *r;                                      // results
UINT indx;                                      // results index

volatile VINT *g;   // NB: position of volatile

ALIGN(64) UINT64 cnt0;
ALIGN(64) UINT64 cnt1;
ALIGN(64) UINT64 cnt2;
UINT64 cnt3;  

class ALIGNEDMA {
public:
void* operator new(size_t); // override new
void operator delete(void*); // override delete
};

class QNode : public ALIGNEDMA {
public:
volatile int waiting;
volatile QNode *next;
};

void* ALIGNEDMA::operator new(size_t sz) // aligned memory allocator
{
sz = (sz + lineSz - 1) / lineSz * lineSz; // make sz a multiple of lineSz
return _aligned_malloc(sz, lineSz); // allocate on a lineSz boundary
}

void ALIGNEDMA::operator delete(void *p)
{
_aligned_free(p); // free object
}

#define LOCKTYPE	1

void noLock(volatile VINT *g);
void bakeryLock(volatile VINT *g, int pid);
void testAndSetLock(volatile VINT *g);
void testAndTestAndSetLock(volatile VINT *g);
void mcsLock(volatile VINT *g);

#if LOCKTYPE == 0
#define LOCKSTR       "No Lock"
//#define LOCKINC()	(*gs)++
#define LOCKINC()	(share_inc)++

#elif LOCKTYPE == 1				
#define LOCKSTR	"Bakery"					
#define LOCKINC()							\
	int mt = worker_nt;						\
	choosing[thread] = true;				\
		/*memory fence ensures value of choosing is written through*/\
	_mm_mfence();							\
	int mx = 0;								\
	for(int i = 0; i<mt; i++){				\
		int num = number[i];				\
		if(num>mx){							\
	mx = num;}								\
	}										\
	number[thread] = mx+1;					\
	choosing[thread] = false;				\
		/*fence ensure choosing value is written through */\
	_mm_mfence();							\
	for(int j=0; j<mt; j++){				\
		while(choosing[j] == true);			\
		while ((number[j] != 0) && ((number[j] < number[thread]) || ((number[j] == number[thread]) && (j < thread))));\
	}	/*aquired lock*/									\
	(*gs)++;									\
		/*release lock*/										\
	number[thread] = 0;

#elif LOCKTYPE == 2
#define LOCKSTR	"TESTnSET"
#define LOCKINC()							\
	while(InterlockedExchange(&lock, 1));	\
	(*gs)++;								\
	lock = 0;

#elif LOCKTYPE == 3
#define LOCKSTR	"TESTnTESTnSET"
#define	LOCKINC()							\
	do{										\
	while(lock==1)							\
			_mm_pause();					\
	}while(InterlockedExchange(&lock, 1));	\
	(*gs)++;								\
	lock = 0;

#elif LOCKTYPE == 4
#define LOCKSTR	"MCS"
#define LOCKINC()							\
	volatile QNode *qn = new volatile QNode;\
	qn->next = NULL;						\
	volatile QNode *pred = (QNode*) InterlockedExchangePointer((PVOID*) MCSlock, (PVOID) qn);\
	if(pred != NULL)						\
	{										\
		qn->waiting = 1;					\
		pred->next = qn;					\
		while (qn->waiting);				\
	}										\
	/*lock aquired*/ 						\
	(*gs)++;									\
	/*lock release*/						\
	volatile QNode *succ = qn->next;		\
	if (!succ) {							\
		if (!(InterlockedCompareExchangePointer((PVOID*)MCSlock, NULL, (PVOID) qn) == qn))\
		{									\
			do{								\
			succ = qn->next;				\
			}while(!succ);					\
			succ->waiting = 0;				\
		}									\
	}										\
	else									\
	{										\
		succ->waiting = 0;					\
	}										\
	delete qn;
#endif

int volatile *number;
bool volatile *choosing;
int worker_nt=0;
volatile long lock = 0;
volatile QNode **MCSlock;

WORKER worker(void *vthread)
{
    int thread = (int)((size_t) vthread);

    UINT64 n = 0;

    volatile VINT *gs = GINDX(maxThread);

    runThreadOnCPU(thread % ncpu);

    while (1) {

		for (int i = 0; i < NOPS; i++) {
#if LOCKTYPE == 0
	//noLock(gs);
	LOCKINC();
#elif LOCKTYPE  == 1 
	//bakeryLock(gs, thread);
	LOCKINC();
#elif LOCKTYPE == 2
	//testAndSetLock(gs);
	LOCKINC();
#elif LOCKTYPE == 3
	//testAndTestAndSetLock(gs);
	LOCKINC();
#elif LOCKTYPE == 4
	//mcsLock();
	LOCKINC();

#endif
		}

        n += NOPS;
		//cout<<"gs = "<<gs<<" on thread "<<thread<<endl;
        //
        // check if runtime exceeded
        //
        if ((getWallClockMS() - tstart) > NSECONDS*1000)
            break;

    }

    ops[thread] = n;
    return 0;

}

int main()
{
	//
    // get cache info
    //
    lineSz = getCacheLineSz();

	ncpu = getNumberOfCPUs();   // number of logical CPUs
    maxThread = 2 * ncpu;       // max number of threads
	//
    // get date
    //
    char dateAndTime[256];
    getDateAndTime(dateAndTime, sizeof(dateAndTime));

	MCSlock = new volatile QNode*;
	*MCSlock = NULL;
    //
    // console output
    //
    cout << getHostName() << " " << getOSName() << " sharing " << (is64bitExe() ? "(64" : "(32") << "bit EXE)" ;
#ifdef _DEBUG
    cout << " DEBUG";
#else
    cout << " RELEASE";
#endif
    cout << " [" << LOCKSTR << "]" << " NCPUS=" << ncpu << " RAM=" << (getPhysicalMemSz() + GB - 1) / GB << "GB " << dateAndTime << endl;
#ifdef COUNTER64
    cout << "COUNTER64";
#else
    cout << "COUNTER32";
#endif
    cout << " NOPS=" << NOPS << " NSECONDS=" << NSECONDS << " LOCKTYPE=" << LOCKSTR;
    cout << endl;
    cout << "Intel" << (cpu64bit() ? "64" : "32") << " family " << cpuFamily() << " model " << cpuModel() << " stepping " << cpuStepping() << " " << cpuBrandString() << endl;

    //lineSz *= 2;

    if ((&cnt3 >= &cnt0) && (&cnt3 < (&cnt0 + lineSz/sizeof(UINT64))))
        cout << "Warning: cnt3 shares cache line used by cnt0" << endl;
    if ((&cnt3 >= &cnt1) && (&cnt3 < (&cnt1 + lineSz / sizeof(UINT64))))
        cout << "Warning: cnt3 shares cache line used by cnt1" << endl;
    if ((&cnt3 >= &cnt2) && (&cnt3 < (&cnt2 + lineSz / sizeof(UINT64))))
        cout << "Warning: cnt2 shares cache line used by cnt1" << endl;

	    cout << endl;

    //
    // allocate global variable
    //
    // NB: each element in g is stored in a different cache line to stop false sharing
    //
    threadH = (THREADH*) ALIGNED_MALLOC(maxThread*sizeof(THREADH), lineSz);             // thread handles
    ops = (UINT64*) ALIGNED_MALLOC(maxThread*sizeof(UINT64), lineSz);                   // for ops per thread
	g = (VINT*) ALIGNED_MALLOC((maxThread + 1)*lineSz, lineSz);   

	r = (Result*) ALIGNED_MALLOC(5*maxThread*sizeof(Result), lineSz);                   // for results
    memset(r, 0, 5*maxThread*sizeof(Result));                                           // zero

    indx = 0;
	    //
    // use thousands comma separator
    //
    setCommaLocale();

	//
    // header
    //
    cout << "sharing";
    cout << setw(4) << "nt";
    cout << setw(6) << "rt";
    cout << setw(16) << "ops";
    cout << setw(6) << "rel";
	    cout << endl;

    cout << "-------";              // sharing
    cout << setw(4) << "--";        // nt
    cout << setw(6) << "--";        // rt
    cout << setw(16) << "---";      // ops
    cout << setw(6) << "---";       // rel

	cout << endl;

    //
    // boost process priority
    // boost current thread priority to make sure all threads created before they start to run
    //

	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

	    //
    // run tests
    //
    UINT64 ops1 = 1;

	for (int nt = 1; nt <= maxThread; nt *= 2, indx++) 
	{
		share_inc = 0;

#if LOCKTYPE == 1
number = (volatile int*) ALIGNED_MALLOC(nt*sizeof(int), lineSz);
choosing = (volatile bool*) ALIGNED_MALLOC(nt*sizeof(bool), lineSz);
worker_nt = nt;
#endif

            for (int thread = 0; thread < nt; thread++)
                *(GINDX(thread)) = 0;   // thread local

            *(GINDX(maxThread)) = 0;    // shared

			            //
            // get start time
            //
            tstart = getWallClockMS();

            //
            // create worker threads
            //
            for (int thread = 0; thread < nt; thread++)
                createThread(&threadH[thread], worker, (void*)(size_t)thread);

            //
            // wait for ALL worker threads to finish
            //
            waitForThreadsToFinish(nt, threadH);
            UINT64 rt = getWallClockMS() - tstart;

			            //
            // save results and output summary to console
            //
            for (int thread = 0; thread < nt; thread++) {
                r[indx].ops += ops[thread];
                r[indx].incs += *(GINDX(thread));
				 }
            r[indx].incs += *(GINDX(maxThread));
			r[indx].incs += share_inc;
            if ((sharing == 0) && (nt == 1))
                ops1 = r[indx].ops;
            r[indx].sharing = sharing;
            r[indx].nt = nt;
            r[indx].rt = rt;

            cout << setw(6) << sharing << "%";
            cout << setw(4) << nt;
            cout << setw(6) << fixed << setprecision(2) << (double) rt / 1000;
            cout << setw(16) << r[indx].ops;
            cout << setw(6) << fixed << setprecision(2) << (double) r[indx].ops / ops1;

			if (r[indx].ops != r[indx].incs)
                cout << " ERROR incs " << setw(3) << fixed << setprecision(0) << 100.0 * r[indx].incs / r[indx].ops << "% effective";

            cout << endl;

            //
            // delete thread handles
            //
            for (int thread = 0; thread < nt; thread++)
                closeThread(threadH[thread]);

        }
	    cout << endl;

		    //
    // output results so they can easily be pasted into a spread sheet from console window
    //
    setLocale();
    cout << "sharing/nt/rt/ops/incs";
	cout << endl;
    for (UINT i = 0; i < indx; i++) {
        cout << r[i].sharing << "/"  << r[i].nt << "/" << r[i].rt << "/"  << r[i].ops << "/" << r[i].incs;
		cout << endl;
    }
    cout << endl;

	    quit();

    return 0;

}
#pragma optimize("", on);
void noLock(volatile VINT *g){
	(*g)++;
}
#pragma optimize("", off);

#pragma optimize("", on);
void bakeryLock(volatile VINT *g, int pid){
	int mt = worker_nt;						
	choosing[pid] = true;	
	_mm_mfence();
	int mx = 0;		
	for(int i = 0; i<mt; i++){							
		int num = number[i];																
		if(num>mx){										
	mx = num;}									
	}																									
	number[pid] = mx+1;									
	choosing[pid] = false;	
	_mm_mfence();
	for(int j=0; j<mt; j++){						
		while(choosing[j] == true);														
		while ((number[j] != 0) && ((number[j] < number[pid]) || ((number[j] == number[pid]) && (j < pid))));
	}													
	(*g)++;												
	number[pid] = 0;															
}
#pragma optimize("", off);

#pragma optimize("", on);
void testAndSetLock(volatile VINT *g)
{														
	while(InterlockedExchange(&lock, 1));					
	(*g)++;													
	lock = 0;												
}	
#pragma optimize("", off);

#pragma optimize("", on);
void testAndTestAndSetLock(volatile VINT *g)
{														
	do{														
		while(lock==1)										
			_mm_pause();									
	}while(InterlockedExchange(&lock, 1));					
	(*g)++;													
	lock = 0;												
}
#pragma optimize("", off);

#pragma optimize("", on);
void mcsLock(volatile VINT *g)
{
	volatile QNode *qn = new volatile QNode;
	qn->next = NULL;								
	volatile QNode *pred = (QNode*) InterlockedExchangePointer((PVOID*) MCSlock, (PVOID) qn);								
	if(pred != NULL)									
	{								
		qn->waiting = 1;			
		pred->next = qn;							
		while (qn->waiting);					
	}	
	//lock aquired 						
	(*g)++;												
	//lock release						
	volatile QNode *succ = qn->next;					
	if (!succ) {										
		if (!(InterlockedCompareExchangePointer((PVOID*)MCSlock, NULL, (PVOID) qn) == qn))
		{												
			do{											
			succ = qn->next;							
			}while(!succ);								
			succ->waiting = 0;							
		}												
	}
	else
	{
		succ->waiting = 0;
	}

	delete qn;
}
#pragma optimize("", off);
