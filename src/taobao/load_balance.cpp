/*

from  http://www.searchtb.com/2014/11/%E8%81%8A%E8%81%8A%E5%A4%9A%E7%BA%BF%E7%A8%8B%E7%A8%8B%E5%BA%8F%E7%9A%84load-balance.html

case-6，采用SERIAL_CALC运算逻辑，重跑case-5中的calc任务
$g++ cond.cpp -pthread -O2 -DSERIAL_CALC
$./a.out -j calc -t 24 -o 12 -a 1 -l
total cost: 51269
$./a.out -j calc -t 24 -o 12 -a 2 -l
total cost: 56753

*/
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sched.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <math.h>
#include <sys/syscall.h>

#define CPUS    24
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET   10

struct Job
{
    long _input;
    long _output;
};

class JobRunner
{
public:
    virtual void run(Job* job) = 0;
};

class ShmJobRunner : public JobRunner
{
public:
    ShmJobRunner(const char* filepath, size_t length)
            : _length(length) {
        int fd = open(filepath, O_RDONLY);
        _base = (long*)mmap(NULL, _length*sizeof(long),
                PROT_READ, MAP_SHARED|MAP_POPULATE, fd, 0);
        if (_base == MAP_FAILED) {
            printf("FATAL: mmap %s(%lu) failed!\n",
                    filepath, _length*sizeof(long));
            abort();
        }
        close(fd);
    }
    virtual void run(Job* job) {
        long i = job->_input % _length;
        long j = i + _length - 1;
        const int step = 4;
        while (i + step < j) {
            if (_base[i%_length] * _base[j%_length] > 0) {
                j -= step;
            }
            else {
                i += step;
            }
        }
        job->_output = _base[i%_length];
    }
private:
    const long* _base;
    size_t _length;
};

class CalcJobRunner : public JobRunner
{
public:
    virtual void run(Job* job) {
        long v1 = 1;
        long v2 = 1;
        long v3 = 1;
        for (int i = 0; i < job->_input; i++) {
#ifndef SERIAL_CALC
            v1 += v2 + v3;
            v2 *= 3;
            v3 *= 5;
#else
            v1 += v2 + v3;
            v2 = v1 * 5 + v2 * v3;
            v3 = v1 * 3 + v1 * v2;
#endif
        }
        job->_output = v1;
    }
};

class JobRunnerCreator
{
public:
    static JobRunner* create(const char* name,
            const char* filepath, size_t filelength) {
        if (strcmp(name, "shm") == 0) {
            printf("share memory job\n");
            return new ShmJobRunner(filepath, filelength);
        }
        else if (strcmp(name, "calc") == 0) {
            printf("caculation job\n");
            return new CalcJobRunner();
        }
        printf("unknown job '%s'\n", name);
        return NULL;
    }
};

class Cond
{
public:
    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual void wait(size_t) = 0;
    virtual void wake() = 0;
};

class NormalCond : public Cond
{
public:
    NormalCond() {
        pthread_mutex_init(&_mutex, NULL);
        pthread_cond_init(&_cond, NULL);
    }
    ~NormalCond() {
        pthread_mutex_destroy(&_mutex);
        pthread_cond_destroy(&_cond);
    }
    void lock() { pthread_mutex_lock(&_mutex); }
    void unlock() { pthread_mutex_unlock(&_mutex); }
    void wait(size_t) { pthread_cond_wait(&_cond, &_mutex); }
    void wake() { pthread_cond_signal(&_cond); }
private:
    pthread_mutex_t _mutex;
    pthread_cond_t _cond;
};

class LayeredCond : public Cond
{
public:
    LayeredCond(size_t layers = 1) : _value(0), _layers(layers) {
        pthread_mutex_init(&_mutex, NULL);
        if (_layers > sizeof(int)*8) {
            printf("FATAL: cannot support such layer %u (max %u)\n",
                    _layers, sizeof(int)*8);
            abort();
        }
        _waiters = new size_t[_layers];
        memset(_waiters, 0, sizeof(size_t)*_layers);
    }
    ~LayeredCond() {
        pthread_mutex_destroy(&_mutex);
        delete _waiters;
        _waiters = NULL;
    }
    void lock() {
        pthread_mutex_lock(&_mutex);
    }
    void unlock() {
        pthread_mutex_unlock(&_mutex);
    }
    void wait(size_t layer) {
        if (layer >= _layers) {
            printf("FATAL: layer overflow (%u/%u)\n", layer, _layers);
            abort();
        }
        _waiters[layer]++;
        while (_value == 0) {
            int value = _value;
            unlock();
            syscall(__NR_futex, &_value, FUTEX_WAIT_BITSET, value,
                    NULL, NULL, layer2mask(layer));
            lock();
        }
        _waiters[layer]--;
        _value--;
    }
    void wake() {
        int mask = ~0;
        lock();
        for (size_t i = 0; i < _layers; i++) {
            if (_waiters[i] > 0) {
                mask = layer2mask(i);
                break;
            }
        }
        _value++;
        unlock();
        syscall(__NR_futex, &_value, FUTEX_WAKE_BITSET, 1,
                NULL, NULL, mask);
    }
private:
    int layer2mask(size_t layer) {
        return 1 << layer;
    }
private:
    pthread_mutex_t _mutex;
    int _value;
    size_t* _waiters;
    size_t _layers;
};

template<class T>
class Stack
{
public:
    Stack(size_t size, size_t cond_layers = 0) : _size(size), _sp(0) {
        _buf = new T*[_size];
        _cond = (cond_layers > 0) ?
                (Cond*)new LayeredCond(cond_layers) : (Cond*)new NormalCond();
    }
    ~Stack() {
        delete []_buf;
        delete _cond;
    }
    T* pop(size_t layer = 0) {
        T* ret = NULL;
        _cond->lock();
        do {
            if (_sp > 0) {
                ret = _buf[--_sp];
            }
            else {
                _cond->wait(layer);
            }
        } while (ret == NULL);
        _cond->unlock();
        return ret;
    }
    void push(T* obj) {
        _cond->lock();
        if (_sp >= _size) {
            printf("FATAL: stack overflow\n");
            abort();
        }
        _buf[_sp++] = obj;
        _cond->unlock();
        _cond->wake();
    }
private:
    const size_t _size;
    size_t _sp;
    T** _buf;
    Cond* _cond;
};

inline struct timeval cost_begin()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv;
}

inline long cost_end(struct timeval &tv)
{
    struct timeval tv2;
    gettimeofday(&tv2, NULL);
    tv2.tv_sec -= tv.tv_sec;
    tv2.tv_usec -= tv.tv_usec;
    return tv2.tv_sec*1000+tv2.tv_usec/1000;
}

struct ThreadParam
{
    size_t layer;
    Stack<Job>* inputQ;
    Stack<Job>* outputQ;
    JobRunner* runner;
};

void* thread_func(void *data)
{
    size_t layer = ((ThreadParam*)data)->layer;
    Stack<Job>* inputQ = ((ThreadParam*)data)->inputQ;
    Stack<Job>* outputQ = ((ThreadParam*)data)->outputQ;
    JobRunner* runner = ((ThreadParam*)data)->runner;

    while (1) {
        Job* job = inputQ->pop(layer);
        runner->run(job);
        outputQ->push(job);
    }
    return NULL;
}

void force_cpu(pthread_t t, int n)
{
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(n, &cpus);
    if (pthread_setaffinity_np(t, sizeof(cpus), &cpus) != 0) {
        printf("FATAL: force cpu %d failed: %s\n", n, strerror(errno));
        abort();
    }
}

void usage(const char* bin)
{
    printf("usage: %s -j job_kind=shm|calc "
            "[-t thread_count=1] [-o job_load=1] [-c job_count=10] "
            "[-a affinity=0] [-l] "
            "[-f filename=\"./TEST\" -n filelength=128M]\n", bin);
    abort();
}

int main(int argc, char* const* argv)
{
    int THREAD_COUNT = 1;
    int JOB_LOAD = 1;
    int JOB_COUNT = 10;
    int AFFINITY = 0;
    int LAYER = 0;
    char JOB_KIND[16] = "";
    char FILEPATH[1024] = "./TEST";
    size_t LENGTH = 128*1024*1024;
    for (int i = EOF;
         (i = getopt(argc, argv, "t:o:c:a:j:lf:n:")) != EOF;) {
        switch (i) {
            case 't': THREAD_COUNT = atoi(optarg); break;
            case 'o': JOB_LOAD = atoi(optarg); break;
            case 'c': JOB_COUNT = atoi(optarg); break;
            case 'a': AFFINITY = atoi(optarg); break;
            case 'l': LAYER = 2; break;
            case 'j': strncpy(JOB_KIND, optarg, sizeof(JOB_KIND)-1); break;
            case 'f': strncpy(FILEPATH, optarg, sizeof(FILEPATH)-1); break;
            case 'n': LENGTH = atoi(optarg); break;
            default: usage(argv[0]); break;
        }
    }
    JobRunner* runner = JobRunnerCreator::create(
            JOB_KIND, FILEPATH, LENGTH);
    if (!runner) {
        usage(argv[0]);
    }

    srand(0);
    Job jobs[JOB_LOAD];

#ifdef TEST_LOAD
    for (int i = 0; i < JOB_LOAD; i++) {
        jobs[i]._input = rand();
        struct timeval tv = cost_begin();
        runner->run(&jobs[i]);
        long cost = cost_end(tv);
        printf("job[%d](%ld)=(%ld) costs: %ld\n",
                i, jobs[i]._input, jobs[i]._output, cost);
    }
    delete runner;
    return 0;
#endif

    printf("use layer %d\n", LAYER);
    Stack<Job> inputQ(JOB_LOAD, LAYER);
    Stack<Job> outputQ(JOB_LOAD, LAYER);

    pthread_t t;
    ThreadParam param[THREAD_COUNT];

    printf("thread init: ");
    for (int i = 0; i < THREAD_COUNT; i++) {
        int cpu = AFFINITY ? (i/AFFINITY+i%AFFINITY*CPUS/2)%CPUS : -1;
        size_t layer = !!(LAYER && i % CPUS >= CPUS/2);
        param[i].inputQ = &inputQ;
        param[i].outputQ = &outputQ;
        param[i].runner = runner;
        param[i].layer = layer;
        pthread_create(&t, NULL, thread_func, (void*)&param[i]);
        if (cpu >= 0) {
            printf("%d(%d|%d),", i, cpu, layer);
            force_cpu(t, cpu);
        }
        else {
            printf("%d(*|%d),", i, layer);
        }
        usleep(1000);
    }
    printf("\n");

    struct timeval tv = cost_begin();
    for (int i = 0; i < JOB_LOAD; i++) {
        jobs[i]._input = rand();
        inputQ.push(&jobs[i]);
    }
    for (int i = 0; i < JOB_LOAD*JOB_COUNT; i++) {
        Job* job = outputQ.pop();
        job->_input = rand();
        inputQ.push(job);
    }
    for (int i = 0; i < JOB_LOAD; i++) {
        outputQ.pop();
    }
    long cost = cost_end(tv);
    printf("total cost: %ld\n", cost);

    delete runner;
    return 0;
}