#include "board_audio_resampler.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

void board_resampler_reset(board_resampler_t *s)
{
    if (!s) return;
    memset(s->history, 0, sizeof(s->history));
    s->position = s->phase = 0;
}

int board_resampler_init(board_resampler_t *s, unsigned input_rate, unsigned output_rate)
{
    if (!s || !((input_rate == 24000 && output_rate == 16000) ||
                (input_rate == 16000 && output_rate == 24000))) return -1;
    memset(s, 0, sizeof(*s));
    s->up = 48000 / input_rate;
    s->down = 48000 / output_rate;
    /* 7.2 kHz cutoff leaves a transition band before the 16 kHz Nyquist limit.
     * Normalize every polyphase branch to unit DC gain, including startup. */
    float coefficients[BOARD_RESAMPLE_TAPS];
    const double pi = 3.14159265358979323846;
    for (unsigned k = 0; k < BOARD_RESAMPLE_TAPS; ++k) {
        double x = (double)k - (BOARD_RESAMPLE_TAPS - 1) / 2.0;
        double sinc = x == 0 ? 0.3 : sin(0.3 * pi * x) / (pi * x);
        double window = 0.54 - 0.46 * cos(2 * pi * k / (BOARD_RESAMPLE_TAPS - 1));
        coefficients[k] = (float)(sinc * window);
    }
    for (unsigned p = 0; p < s->up; ++p) {
        double sum = 0;
        for (unsigned k = p; k < BOARD_RESAMPLE_TAPS; k += s->up) sum += coefficients[k];
        for (unsigned k = p; k < BOARD_RESAMPLE_TAPS; k += s->up) coefficients[k] /= (float)sum;
    }
    for(unsigned k=0;k<BOARD_RESAMPLE_TAPS;k++) {
        float value=coefficients[k]*32768.0f;
        s->coefficients[k]=(int32_t)(value<0?value-0.5f:value+0.5f);
    }
    /* Sum(abs(Q15 taps)) per branch <=60082 for these two fixed filters;
     * 60082*32768=1968766976 < INT32_MAX, including full-scale input. */
    for(unsigned p=0;p<s->up;p++) {
        uint32_t bound=0;
        for(unsigned k=p;k<BOARD_RESAMPLE_TAPS;k+=s->up)
            bound+=(uint32_t)(s->coefficients[k]<0?-s->coefficients[k]:s->coefficients[k]);
        if(bound>65535)return -1;
    }
    return 0;
}

/* State/history aliases retain the original per-step state semantics. */
static int process_original(board_resampler_t *s,const int16_t *input,size_t n,int16_t *output)
{
    size_t produced = 0;
    for (size_t i = 0; i < n; ++i) {
        for (unsigned u = 0; u < s->up; ++u) {
            s->history[s->position] = u == 0 ? input[i] : 0;
            if (s->phase == 0) {
                int32_t sum = 0;
                /* Only every up-th history entry is real input. The other
                 * polyphase branches contain exact inserted zeros; skip them
                 * without changing filter coefficients, latency or phase. */
                unsigned h = s->position >= u ? s->position - u : s->position + BOARD_RESAMPLE_TAPS - u;
                for (unsigned k = u; k < BOARD_RESAMPLE_TAPS; k += s->up) {
                    sum += s->history[h] * s->coefficients[k];
                    h = h >= s->up ? h - s->up : h + BOARD_RESAMPLE_TAPS - s->up;
                }
                int32_t rounded=sum<0?-(((-sum)+16384)/32768):(sum+16384)/32768;
                if (rounded > 32767) rounded = 32767;
                if (rounded < -32768) rounded = -32768;
                output[produced++] = (int16_t)rounded;
            }
            /* Init/reset and every prior step keep both counters in range.
             * Preserve exact phase/history without division in the sample loop. */
            if (++s->position == BOARD_RESAMPLE_TAPS) s->position = 0;
            if (++s->phase == s->down) s->phase = 0;
        }
    }
    return (int)produced;
}

/* Restrict applies only to state. Input/output may still alias each other.
 * The caller verifies that neither data span overlaps the state object. */
static int process_disjoint(board_resampler_t *restrict s,const int16_t *input,size_t n,int16_t *output)
{
    unsigned position=s->position,phase=s->phase;
    const unsigned up=s->up,down=s->down;
    size_t produced = 0;
    for (size_t i = 0; i < n; ++i) {
        for (unsigned u = 0; u < up; ++u) {
            s->history[position] = u == 0 ? input[i] : 0;
            if (phase == 0) {
                int32_t sum = 0;
                /* Only every up-th history entry is real input. The other
                 * polyphase branches contain exact inserted zeros; skip them
                 * without changing filter coefficients, latency or phase. */
                unsigned h = position >= u ? position - u : position + BOARD_RESAMPLE_TAPS - u;
                for (unsigned k = u; k < BOARD_RESAMPLE_TAPS; k += up) {
                    sum += s->history[h] * s->coefficients[k];
                    h = h >= up ? h - up : h + BOARD_RESAMPLE_TAPS - up;
                }
                int32_t rounded=sum<0?-(((-sum)+16384)/32768):(sum+16384)/32768;
                if (rounded > 32767) rounded = 32767;
                if (rounded < -32768) rounded = -32768;
                output[produced++] = (int16_t)rounded;
            }
            /* Init/reset and every prior step keep both counters in range.
             * Preserve exact phase/history without division in the sample loop. */
            if (++position == BOARD_RESAMPLE_TAPS) position = 0;
            if (++phase == down) phase = 0;
        }
    }
    s->position=position;s->phase=phase;
    return (int)produced;
}

/* Fixed-rate disjoint path: each circular dot product crosses the ring at
 * most once. Split at that boundary so every tap need not test for wrapping.
 * Accumulate in the original coefficient order, with no temporary buffers. */
static inline __attribute__((always_inline)) int process_fixed(
    board_resampler_t *restrict s,const int16_t *input,size_t n,int16_t *output,
    unsigned up,unsigned down)
{
    unsigned position=s->position,phase=s->phase;
    size_t produced=0;
    for(size_t i=0;i<n;++i) {
        for(unsigned u=0;u<up;++u) {
            s->history[position]=u==0?input[i]:0;
            if(phase==0) {
                unsigned h=position>=u?position-u:position+BOARD_RESAMPLE_TAPS-u;
                unsigned taps=(BOARD_RESAMPLE_TAPS-u+up-1)/up;
                unsigned first=h/up+1;
                if(first>taps)first=taps;
                unsigned k=u;
                int32_t sum=0;
                for(unsigned j=0;j<first;++j) {
                    sum+=s->history[h]*s->coefficients[k];
                    h-=up;k+=up;
                }
                /* Unsigned wrap here is defined; the second segment starts
                 * at the original loop's first wrapped history index. */
                h+=BOARD_RESAMPLE_TAPS;
                for(unsigned j=first;j<taps;++j) {
                    sum+=s->history[h]*s->coefficients[k];
                    h-=up;k+=up;
                }
                int32_t rounded=sum<0?-(((-sum)+16384)/32768):(sum+16384)/32768;
                if(rounded>32767)rounded=32767;
                if(rounded<-32768)rounded=-32768;
                output[produced++]=(int16_t)rounded;
            }
            if(++position==BOARD_RESAMPLE_TAPS)position=0;
            if(++phase==down)phase=0;
        }
    }
    s->position=position;s->phase=phase;
    return (int)produced;
}

/* The 24k->16k filter has two individually symmetric polyphase branches.
 * Pair their real input samples before multiplying: 16 rather than 32/31
 * multiplies per output. Integer addition is exact; the existing absolute
 * coefficient bound also bounds every partial paired sum. No history,
 * rounding, latency or coefficient changes, and no extra working storage. */
static int process_symmetric_down(board_resampler_t *restrict s,
    const int16_t *input,size_t n,int16_t *output)
{
    unsigned position=s->position,phase=s->phase;
    size_t produced=0;
    for(size_t i=0;i<n;++i)for(unsigned u=0;u<2;++u) {
        s->history[position]=u==0?input[i]:0;
        if(phase==0) {
            unsigned a=position>=u?position-u:position+BOARD_RESAMPLE_TAPS-u;
            const unsigned taps=u==0?32:31;
            unsigned b=a>=2*(taps-1)?a-2*(taps-1):a+BOARD_RESAMPLE_TAPS-2*(taps-1);
            int32_t sum=0;
            unsigned j=0;
            while(j<taps/2) {
                /* Each paired traversal crosses the ring at most once.
                 * Split there, leaving no wrap branches inside the dot product. */
                unsigned run=a/2+1,forward=(BOARD_RESAMPLE_TAPS-1-b)/2+1;
                if(run>forward)run=forward;
                if(run>taps/2-j)run=taps/2-j;
                for(unsigned k=0;k<run;++k) {
                    sum+=((int32_t)s->history[a]+s->history[b])*s->coefficients[u+2*j];
                    a-=2;b+=2;++j;
                }
                if(a>=BOARD_RESAMPLE_TAPS)a+=BOARD_RESAMPLE_TAPS;
                if(b>=BOARD_RESAMPLE_TAPS)b-=BOARD_RESAMPLE_TAPS;
            }
            if(taps&1)sum+=s->history[a]*s->coefficients[u+2*(taps/2)];
            int32_t rounded=sum<0?-(((-sum)+16384)/32768):(sum+16384)/32768;
            if(rounded>32767)rounded=32767;
            if(rounded<-32768)rounded=-32768;
            output[produced++]=(int16_t)rounded;
        }
        if(++position==BOARD_RESAMPLE_TAPS)position=0;
        if(++phase==3)phase=0;
    }
    s->position=position;s->phase=phase;
    return (int)produced;
}

/* An input/output span may alias history in the public state object.
 * Preserve that behavior by using the original state for such calls. */
static int aliases_state(const board_resampler_t *s,const int16_t *data,size_t n)
{
    if (!n) return 0;
    uintptr_t state=(uintptr_t)s, samples=(uintptr_t)data;
    return samples>=state ? samples-state<sizeof(*s) :
        (state-samples)/sizeof(*data)<n;
}

int board_resampler_process(board_resampler_t *s, const int16_t *input, size_t n,
                            int16_t *output, size_t capacity)
{
    if (!s || !input || !output || !s->up || !s->down || n > (SIZE_MAX - 3) / s->up) return -1;
    size_t ticks = n * s->up;
    unsigned first = (s->down - s->phase) % s->down;
    size_t count = ticks > first ? 1 + (ticks - first - 1) / s->down : 0;
    if (capacity < count || count > INT32_MAX) return -1;
    if (aliases_state(s,input,n) || aliases_state(s,output,count))
        return process_original(s,input,n,output);
    if(s->up==2 && s->down==3) {
        bool symmetric=true;
        for(unsigned k=0;k<BOARD_RESAMPLE_TAPS/2;k++)
            if(s->coefficients[k]!=s->coefficients[BOARD_RESAMPLE_TAPS-1-k]){symmetric=false;break;}
        if(symmetric)return process_symmetric_down(s,input,n,output);
        return process_fixed(s,input,n,output,2,3);
    }
    if(s->up==3 && s->down==2)return process_fixed(s,input,n,output,3,2);
    return process_disjoint(s,input,n,output);
}

int board_resampler_process_mmr24(board_resampler_t *restrict m0,board_resampler_t *restrict m1,
    board_resampler_t *restrict r,const int16_t *stereo,const int16_t *ref,size_t n,bool mute,
    int16_t *out0,int16_t *out1,int16_t *out_ref,size_t capacity)
{
    if(!m0||!m1||!r||!stereo||!ref||!out0||!out1||!out_ref||
       m0==m1||m0==r||m1==r||n>(SIZE_MAX-3)/2||
       m0->up!=2||m0->down!=3||m1->up!=2||m1->down!=3||r->up!=2||r->down!=3||
       m0->position>=BOARD_RESAMPLE_TAPS||m0->phase>=3||
       m0->position!=m1->position||m0->position!=r->position||
       m0->phase!=m1->phase||m0->phase!=r->phase)return -1;
    size_t ticks=n*2;unsigned first=(3-m0->phase)%3;
    size_t count=ticks>first?1+(ticks-first-1)/3:0;
    if(capacity<count||count>INT32_MAX)return -1;
    if(memcmp(m0->coefficients,m1->coefficients,sizeof(m0->coefficients))||
       memcmp(m0->coefficients,r->coefficients,sizeof(m0->coefficients)))return -1;
    for(unsigned k=0;k<BOARD_RESAMPLE_TAPS/2;k++)
        if(m0->coefficients[k]!=m0->coefficients[BOARD_RESAMPLE_TAPS-1-k])return -1;
    /* AFE-only disjoint arrays. Share phase/index/coefficients, never stream
     * histories, sums, rounding or the real playback reference. */
    unsigned position=m0->position,phase=m0->phase;
    size_t produced=0;
    for(size_t i=0;i<n;++i)for(unsigned u=0;u<2;++u) {
        m0->history[position]=u==0&&!mute?stereo[2*i]:0;
        m1->history[position]=u==0&&!mute?stereo[2*i+1]:0;
        r->history[position]=u==0?ref[i]:0;
        if(phase==0) {
            unsigned a=position>=u?position-u:position+BOARD_RESAMPLE_TAPS-u;
            const unsigned taps=u==0?32:31;
            unsigned b=a>=2*(taps-1)?a-2*(taps-1):a+BOARD_RESAMPLE_TAPS-2*(taps-1);
            int32_t sum0=0,sum1=0,sumr=0;
            unsigned j=0;
            while(j<taps/2) {
                /* Each paired traversal crosses the ring at most once.
                 * Split there, leaving no wrap branches inside the dot product. */
                unsigned run=a/2+1,forward=(BOARD_RESAMPLE_TAPS-1-b)/2+1;
                if(run>forward)run=forward;
                if(run>taps/2-j)run=taps/2-j;
                for(unsigned k=0;k<run;++k) {
                    int32_t c=m0->coefficients[u+2*j];
                    sum0+=((int32_t)m0->history[a]+m0->history[b])*c;
                    sum1+=((int32_t)m1->history[a]+m1->history[b])*c;
                    sumr+=((int32_t)r->history[a]+r->history[b])*c;
                    a-=2;b+=2;++j;
                }
                if(a>=BOARD_RESAMPLE_TAPS)a+=BOARD_RESAMPLE_TAPS;
                if(b>=BOARD_RESAMPLE_TAPS)b-=BOARD_RESAMPLE_TAPS;
            }
            if(taps&1){int32_t c=m0->coefficients[u+2*(taps/2)];sum0+=m0->history[a]*c;sum1+=m1->history[a]*c;sumr+=r->history[a]*c;}
            {int32_t rounded=sum0<0?-(((-sum0)+16384)/32768):(sum0+16384)/32768;
            if(rounded>32767)rounded=32767;
            if(rounded<-32768)rounded=-32768;
            out0[produced]=(int16_t)rounded;}
            {int32_t rounded=sum1<0?-(((-sum1)+16384)/32768):(sum1+16384)/32768;
            if(rounded>32767)rounded=32767;
            if(rounded<-32768)rounded=-32768;
            out1[produced]=(int16_t)rounded;}
            {int32_t rounded=sumr<0?-(((-sumr)+16384)/32768):(sumr+16384)/32768;
            if(rounded>32767)rounded=32767;
            if(rounded<-32768)rounded=-32768;
            out_ref[produced]=(int16_t)rounded;}
            ++produced;
        }
        if(++position==BOARD_RESAMPLE_TAPS)position=0;
        if(++phase==3)phase=0;
    }
    m0->position=m1->position=r->position=position;
    m0->phase=m1->phase=r->phase=phase;
    return (int)produced;
}
