#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ltc.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <unistd.h>
#include <wiringPi.h>
#include "ringbuf.h"
#include "log.h"
#include "timef.h"

#define SIG SIGRTMIN
#define errExit(msg)		do { log_error(msg); exit(EXIT_FAILURE); } while (0);

#define LTC_ANNOUNCE_PORT 49878
/*
 *
 * this version of the timer uses a separate thread for the LTC encoder which is
 * synchronized using pthread_barriers and mutexes.
 *
 * A timing loop in a second thread is used to signal the process which then 
 * alters the waveform edge at the correct time (once every 250ms) for 25fps.
 *
 * The encoder thread is free-running and advanced by the signal handler every
 * 160 edges (80 bits = 1 frame)
 *
 */

void		*dst;
volatile int	iOutCount=0;
volatile int 	bCount=0;
volatile char	*pOutChar;
pthread_mutex_t tMutexRingBuffer;
pthread_barrier_t ptBarrier;
pthread_barrier_t ptStartBarrier;
pthread_t	encoder_thread;
pthread_t	timer_thread;
ringbuf_t	pRingBuffer;
SMPTETimecode 	smpte;
LTCEncoder 	*encoder;
SMPTETimecode 	st;

struct sockaddr_in app_addr;
int app_addrlen, app_sock;
struct in_addr app_in_addr;

void setupLTCAnnounceSocket() {
	/* set up socket */
	app_sock = socket(AF_INET, SOCK_DGRAM, 0);
	int broadcastEnable=1;
	int ret=setsockopt(app_sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
	if (app_sock < 0) {
		log_error("failed to create app_socket");
		exit(1);
	}
	bzero((char *)&app_addr, sizeof(app_addr));
	app_addr.sin_family = AF_INET;
	app_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	app_addr.sin_addr.s_addr = inet_addr("192.168.4.255");
	app_addr.sin_port = htons(LTC_ANNOUNCE_PORT);
	app_addrlen = sizeof(app_addr);
}

void ltc_announce() {
	struct timespec tp;
	uint64_t ttm;
	char buf[1024];

	clock_gettime(CLOCK_REALTIME,&tp);

	struct tm ltcm;
	time_t tt=tp.tv_sec;

	localtime_r(&tt,&ltcm);

	ttm=timespec_to_uint64(&tp);
	snprintf(buf,1024,"TIMESYNC:%llu:%02u.%02u.%02u.%02u",ttm,ltcm.tm_hour,ltcm.tm_min,ltcm.tm_sec,0);

	log_info("announcing presence for time: %02u:%02u:%02u:%02u",ltcm.tm_hour,ltcm.tm_min,ltcm.tm_sec,0);

	if (sendto(app_sock,buf,strlen(buf),0,(struct sockaddr *) &app_addr,app_addrlen)==-1) {
		log_error("sendto error during announce: %s",strerror(errno));
	}
}
/*
 * toggle the GPIO pin depending on the sample value
 */
void 
setGPIOPin(volatile char * value) {
	if ((*value)<0xA0) {
		digitalWrite(0,LOW);
	} else {
		digitalWrite(0,HIGH);
	}
}

void 
setGPIOPin2(volatile char * value) {
	if (!(*value)) {
		digitalWrite(0,LOW);
	} else {
		digitalWrite(0,HIGH);
	}
}

/*
 * a Handler is polled for every sample 
 *
 * We are trying to keep the structire of 
 * the signal handler identical for every edge
 * so as to make the timing more consistent.
 *
 * Mutex may still be an issue here, however
 * we are only grabbing one byte per edge now 
 * rather than 16 in a go then free-running on the 
 * subsequent 15.
 *
 */

int edge=0;

static void
signal_handler(int sig, siginfo_t *si, void *uc)
{

	if (iOutCount == 0) {

		pthread_mutex_lock(&tMutexRingBuffer);
		ringbuf_memcpy_from(dst,pRingBuffer,160);	
		pthread_mutex_unlock(&tMutexRingBuffer);

	}
	setGPIOPin2((char*)(dst + iOutCount));
	iOutCount = (iOutCount + 1) % 160;

}

/*
 * Encoder Thread Function
 */
void *encoder_thread_function(void*args) {

	ltcsnd_sample_t *buf;
	int byte_cnt;
	int len;
	size_t bufsize=0;

	/* 
	 * setup CPU affinity
	 *
	 * get this to run on a single CPU so that there isn't any contention for resources (hopefully)
	 */

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(2,&cpuset);
	pthread_setaffinity_np(pthread_self(),sizeof(cpuset),&cpuset);

	/*
	 * Wait here until the timer gets going then we start encoding
	 */
	pthread_barrier_wait(&ptStartBarrier);

	/*
	 * main encoder loop
	 */
	while (1) {

		/*
		 * 10 bytes per frame 
		 * 80 bits per frame
		 * 8 bits per byte
		 * 2 samples per bit
		 */


		digitalWrite(3,HIGH);

		ltc_encoder_encode_frame(encoder);

		buf = ltc_encoder_get_bufptr(encoder, &len, 0);

		pthread_mutex_lock(&tMutexRingBuffer);
		ringbuf_memcpy_into(pRingBuffer,buf,len);
		pthread_mutex_unlock(&tMutexRingBuffer);

		ltc_encoder_buffer_flush(encoder);

		while(1) {
			pthread_mutex_lock(&tMutexRingBuffer);
			bufsize=ringbuf_bytes_used(pRingBuffer);
			pthread_mutex_unlock(&tMutexRingBuffer);
			if (bufsize<640)
				break;
			usleep(10000);
		}

		digitalWrite(3,LOW);

		ltc_encoder_inc_timecode(encoder);
	}

	ltc_encoder_free(encoder);

	pthread_exit(NULL);
}

/*
 * Here we initialize the encoder
 */
void
initialize_encoder(struct timespec * ts,int reinit) {

	time_t tv;
	struct tm ltcm;
	double sampleRate=4000; //25fps=4000 samples per second
	int fps=25;

	time_t tt=ts->tv_sec;
	localtime_r(&tt,&ltcm);

	/* start encoding at this timecode */
	const char timezone[6] = "+0100";
	strcpy(st.timezone, timezone);
	st.years =  ltcm.tm_year+1900;
	st.months = ltcm.tm_mon;
	st.days =   ltcm.tm_mday;

	st.hours = ltcm.tm_hour;
	st.mins = ltcm.tm_min;
	st.secs = ltcm.tm_sec;

	st.frame = 0;

	log_info("TC:%02u:%02u:%02u:%02u",st.hours,st.mins,st.secs,st.frame);

	if (!reinit)  {
		encoder = ltc_encoder_create(sampleRate, fps,
				fps==25?LTC_TV_625_50:LTC_TV_525_60, LTC_USE_DATE);
	}
	ltc_encoder_set_timecode(encoder, &st);
}

/*
 * Timer Thread Function
 */
void *timer_thread_function(void*args) {
	volatile struct ntptimeval time;
	struct timespec rtime;
	struct tm ltime;
	time_t sse;
	struct sigaction sa;
	volatile long long old_ms;
	volatile long long us;
	volatile long long edge;
	volatile long int framecount=0;
	volatile long int cnt_e=0;
	volatile int encoder_initialized=0;
	volatile int firstime=1;
	volatile int clock_stable=0;
	int len;
	volatile int byte_cnt=0;
	volatile int iOutCount=0;
	void * buf;
	int ledon=0;
	int clkPulse=0;

	/* 
	 * setup CPU affinity
	 */

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(3,&cpuset);

	pthread_setaffinity_np(pthread_self(),sizeof(cpuset),&cpuset);

	/*
	 * Setup the signal handler
	 */
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = signal_handler;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIG, &sa, NULL) == -1)
		errExit("sigaction");

	/*
	 * Main timer loop
	 */

	while (1) {

		/*
		 * Grab the time from the real-time-clock
		 * This will be affected by NTP
		 */
#if defined _v1
		while (1) {
			clock_gettime(CLOCK_REALTIME,&rtime);
			us=rtime.tv_nsec/2000; // 2 - microsecond resolution
			if (us!=old_ms) {
				old_ms=us;
				break;
			}
		}
#else 
			clock_gettime(CLOCK_REALTIME,&rtime);
			us=rtime.tv_nsec/2000; // 2 - microsecond resolution

#endif // _v1

		/*
		 * Setup on first run into loop
		 *
		 * We are going to dump cycles until we hit a second boundary
		 *
		 */
		if (firstime) {
			if (us!=0) {
				continue;
			} else {
				firstime=0;
			}
		}

		/*
		 * Second edge boundary
		 */
		if (us==0) {
			/*
			 * Once the counter hits 4000 samples per 
			 * second the clock is running at speed
			 *
			 * we re-initializethe encoder and 
			 * mark the clock stable so we don't come here again.
			 *
			 * Schroedingers Cat is at work here.
			 *
			 * If I put an else clause after this "if" to check for
			 * conditions when cnt_e doesn't get to 4000, it never ever 
			 * gets to 4000.
			 *
			 * Even a separate If statement covering that condition causes the same issue.
			 *
			 */

			if (cnt_e==4000 ) {
				if (!clock_stable) {
					initialize_encoder(&rtime,1);
					clock_stable=1;
					digitalWrite(1,HIGH);
				} else {
					ledon=1-ledon;
					digitalWrite(4,ledon);
				}
			} else {
					digitalWrite(1,LOW);
			}

			ltc_announce();

			framecount=0;
			cnt_e=0;

			/* 
			 * If we havn't initialised the encoder yet,
			 * we do it here
			 */
			if (!encoder_initialized) {

				/*
				 * Once we have some bytes in the buffer,
				 * indicate the encoder is running and
				 * this will warm up the streamer.
				 */
				initialize_encoder(&rtime,0);

				/*
				 * let the encoder thread know that we are ready to go
				 */
				log_info("Waiting on the encoder");
				pthread_barrier_wait(&ptStartBarrier);
				log_info("Done waiting on the encoder");
				encoder_initialized=1;
			}
		}

		/*
		 * LTC waveform edge
		 */
		if (((us % 125) == 0 ) && encoder_initialized) {
			cnt_e++;
			clkPulse=1-clkPulse;
			digitalWrite(5,clkPulse);
			raise(SIG);
		}

	}
}


int
main (int argc, char *argv[]) {
	log_info("NTP2LTC Timecode Generator v0.1 alpha");
	log_info("Greenside Productions 2018...");

	/*
	 * buffer setup
	 */

	dst = calloc(1280,sizeof(uint8_t));
	pRingBuffer=ringbuf_new(sizeof(uint8_t) * 1280);	
	ringbuf_reset(pRingBuffer);

	/*
	 * Wiring Pi Setup
	 */
	wiringPiSetup();
	pinMode(0,OUTPUT); // GPIO Pin 0 for timecode output
	pinMode(1,OUTPUT); // GPIO Pin 1 for status indication
	pinMode(4,OUTPUT); // GPIO Pin 4 for hearbeat indication
	pinMode(5,OUTPUT); // GPIO Pin 5 for safety clock
	digitalWrite(1,LOW);
	digitalWrite(4,LOW);
	digitalWrite(5,LOW);

	/*
	 * Set up the pthread barrier structure
	 */ 
	pthread_barrier_init(&ptStartBarrier,NULL,2);

	/*
	 * Set up the mutex
	 */
	pthread_mutex_init(&tMutexRingBuffer,NULL);

	/*
	 * Setup Announcement Socket
	 */

	setupLTCAnnounceSocket() ;

	/*
	 * Threads setup
	 */

	pthread_create(&timer_thread,NULL,timer_thread_function,NULL) ;
	pthread_create(&encoder_thread,NULL,encoder_thread_function,NULL) ;

	/*
	 * cleanup
	 */

	pthread_join(encoder_thread,NULL);
	pthread_join(timer_thread,NULL);
	return 0;
}
