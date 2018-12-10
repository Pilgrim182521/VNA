/* VNA

*/

#include <Wire.h>
#include "si5351.h"
#include "tinycl.h"
#include "debugmsg.h"
#include "complex.h"
#include "flashstruct.h"

#define IFCLOCK_PIN PB15
#define IFCLOCK_PIN_CTR PA15
#define IFCLOCK_PORT2_PIN PA8
#define AD_CUR_PIN PA0
#define AD_VOLT_PIN PB0
#define AD_CUR2_PIN PA2
#define ATTEN_PIN PB14

Si5351 si5351;

int pinMapADCCURPINin;
int pinMapADCVOLTPINin;
int pinMapADCCUR2PINin;

void setup_analog(int accurate)
{
  pinMapADCCURPINin = PIN_MAP[AD_CUR_PIN].adc_channel;
  pinMapADCVOLTPINin = PIN_MAP[AD_VOLT_PIN].adc_channel;
  pinMapADCCUR2PINin = PIN_MAP[AD_CUR2_PIN].adc_channel;
  adc_set_sample_rate(ADC1, accurate ? ADC_SMPR_13_5 : ADC_SMPR_1_5);
  adc_set_reg_seqlen(ADC1, 1);
  adc_set_sample_rate(ADC2, accurate ? ADC_SMPR_13_5 : ADC_SMPR_1_5);
  adc_set_reg_seqlen(ADC2, 1);
}

volatile short sampCUR, sampVOLT, sampCUR2;

volatile int number_to_sum = 256;
volatile int current_summed = 0;

volatile int sampCURI, sampVOLTI, sampCUR2I, sampPWR;
volatile int sampCURQ, sampVOLTQ, sampCUR2Q;

unsigned short analogReadPins(void)
{
  ADC1->regs->SQR3 = pinMapADCCURPINin;
  ADC1->regs->CR2 |= ADC_CR2_SWSTART;
  ADC2->regs->SQR3 = pinMapADCVOLTPINin;
  ADC2->regs->CR2 |= ADC_CR2_SWSTART;

  while (!(ADC1->regs->SR & ADC_SR_EOC));
  sampCUR = (ADC1->regs->DR & ADC_DR_DATA);

  ADC1->regs->SQR3 = pinMapADCCUR2PINin;
  ADC1->regs->CR2 |= ADC_CR2_SWSTART;

  while (!(ADC2->regs->SR & ADC_SR_EOC));
  sampVOLT = (ADC2->regs->DR & ADC_DR_DATA);

  while (!(ADC1->regs->SR & ADC_SR_EOC));
  sampCUR2 = (ADC1->regs->DR & ADC_DR_DATA);
}

void setup_pins(void)
{
  pinMode(ATTEN_PIN, OUTPUT);
  digitalWrite(ATTEN_PIN, LOW);
  pinMode(IFCLOCK_PIN, INPUT);
  pinMode(IFCLOCK_PIN_CTR, INPUT);
  pinMode(IFCLOCK_PORT2_PIN, INPUT);
  pinMode(AD_CUR_PIN, INPUT_ANALOG);
  pinMode(AD_VOLT_PIN, INPUT_ANALOG);
  pinMode(AD_CUR2_PIN, INPUT_ANALOG);
}

#define DEMCR           (*((volatile uint32_t *)0xE000EDFC))
#define DWT_CTRL        (*(volatile uint32_t *)0xe0001000)
#define CYCCNTENA       (1<<0)
#define DWT_CYCCNT      ((volatile uint32_t *)0xE0001004)
#define CPU_CYCLES      *DWT_CYCCNT
#define DEMCR_TRCENA    0x01000000

HardwareTimer timeb(3);

static void initialize_cortex_m3_cycle_counter(void)
{
  DEMCR |= DEMCR_TRCENA;
  *DWT_CYCCNT = 0;
  DWT_CTRL |= CYCCNTENA;
}

volatile unsigned int diftick;
volatile unsigned int adclocks;
volatile unsigned int lasttick = 0;

unsigned int numphases = 4;
volatile unsigned int curphase = 0;

unsigned int timephase[30];

void ifClockInterrupt(void)
{
  unsigned int timerval = CPU_CYCLES;

  diftick = ((unsigned int)(timerval - lasttick));
  lasttick = timerval;

  if (current_summed < number_to_sum)
  {
    if (current_summed == 0)
    {
      sampCURI = 0; sampCURQ = 0;
      sampVOLTI = 0; sampVOLTQ = 0;
      sampCUR2I = 0; sampCUR2Q = 0;
      sampPWR = 0;
    }
    current_summed++;
    timephase[0] = timerval;
    timer_pause(TIMER3);
    timer_set_count(TIMER3, 0);
    timerval = diftick / numphases;
    if (timerval > (F_CPU / 200000u))
    {
      timer_set_reload(TIMER3, timerval);
      //    timer_set_compare(TIMER3,1,0);
      timer_generate_update(TIMER3);
      curphase = 0;
      timer_resume(TIMER3);
    }
  }
}

void timerContInterrupt(void)
{
  unsigned int timerval = CPU_CYCLES;

  if (curphase < numphases)
  {
    timephase[curphase] = timerval;
    analogReadPins();
    switch (curphase & 0x03)
    {
      case 0: sampCURI += sampCUR;
        sampVOLTI += sampVOLT;
        sampCUR2I += sampCUR2;
        break;
      case 1: sampCURQ -= sampCUR;
        sampVOLTQ -= sampVOLT;
        sampCUR2Q -= sampCUR2;
        break;
      case 2: sampCURI -= sampCUR;
        sampVOLTI -= sampVOLT;
        sampCUR2I -= sampCUR2;
        break;
      case 3: sampCURQ += sampCUR;
        sampVOLTQ += sampVOLT;
        sampCUR2Q += sampCUR2;
        break;
    }
    sampPWR += sampCUR2;
    curphase++;
  }
}

void setup_if_clock(void)
{
  nvic_irq_set_priority(NVIC_EXTI_15_10, 2);
  attachInterrupt(IFCLOCK_PIN, ifClockInterrupt, RISING);

  nvic_irq_set_priority(NVIC_TIMER3, 2);
  timeb.pause();
  timeb.setCount(0);
  timeb.setPrescaleFactor(1);
  timeb.setOverflow(65535);
  timeb.setCompare(TIMER_CH1, 0);
  timeb.attachInterrupt(1, timerContInterrupt);
  timeb.refresh();
  //timeb.resume();
}


void setup() {
  initialize_cortex_m3_cycle_counter();
  setup_pins();
  setup_if_clock();
  setup_analog(0);

  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 27000000u, 0);
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_6MA);
  si5351.drive_strength(SI5351_CLK1, SI5351_DRIVE_6MA);
  si5351.set_ms_source(SI5351_CLK0, SI5351_PLLA);
  si5351.set_ms_source(SI5351_CLK1, SI5351_PLLB);
  si5351.set_freq(10000000ull * SI5351_FREQ_MULT, SI5351_CLK0);
  si5351.set_freq(10010000ull * SI5351_FREQ_MULT, SI5351_CLK1);
  si5351.output_enable(SI5351_CLK0, 1);
  si5351.output_enable(SI5351_CLK1, 1);
}

#define VNA_MAX_FREQS 80
#define VNA_MIN_FREQ 200000u
#define VNA_MAX_FREQ 470000000u
#define VNA_AUTO_ATTEN_FREQ 60000000u
#define VNA_FREQ_3X 180000000u
#define VNA_NOMINAL_1X_IF_FREQ 10024u
#define VNA_NOMINAL_3X_IF_FREQ (VNA_NOMINAL_1X_IF_FREQ/3)


typedef struct _vna_flash_header
{
  unsigned int flash_header_1;
  unsigned int flash_header_2;
} vna_flash_header;

const vna_flash_header flash_header = { 0xDEADBEEF, 0xC001ACE5 };

typedef enum { VNA_NO_CALIB = 0, VNA_OPEN, VNA_SHORT, VNA_LOAD, VNA_VALID_CALIB_1PT, VNA_VALID_CALIB_2PT, } vna_calib_state;

typedef struct _vna_acquisition_state
{
  unsigned int startfreq;
  unsigned int endfreq;
  unsigned int char_impedance;
  unsigned short num_averages;
  unsigned short timeout;
  unsigned short nfreqs;
  unsigned char csv;
  unsigned char atten;
  vna_calib_state calib_state;
} vna_acquisition_state;

vna_acquisition_state vna_state = {3000000u, 30000000u, 50u, 64, 1000, VNA_MAX_FREQS, 0, 2, VNA_NO_CALIB };

typedef struct _vna_calib_freq_parm
{
  Complex b, c, d;
  Complex z2, i2, s2;
} vna_calib_freq_parm;

typedef struct _vna_calib_oneport
{
  Complex zo, zs, zt;
} vna_calib_oneport;

vna_calib_oneport *vna_1pt = NULL;

vna_calib_freq_parm vna_calib[VNA_MAX_FREQS];

void reset_calib_state(void)
{
  if (vna_1pt != NULL) free(vna_1pt);
  vna_1pt = NULL;
  vna_state.calib_state = VNA_NO_CALIB;
}

int acquire_sample(unsigned int averages, unsigned int timeout)
{
  bool timedout = false;
  current_summed = 0;
  unsigned int inittime, curtime;

  timeout = timeout * (F_CPU / 1000u);
  /* wait for previous acquisition to stop */
  inittime = CPU_CYCLES;
  timedout = false;
  while (current_summed < number_to_sum)
  {
    curtime = CPU_CYCLES;
    if (((unsigned int)(curtime - inittime)) > timeout)
    {
      timedout = true;
      break;
    }
  }
  if (timedout)
  {
    Serial.println("ACQ: Timeout waiting for last acquisition");
    return -1;
  }
  number_to_sum = averages;
  current_summed = 0;
  /* wait for previous acquisition to stop */
  inittime = CPU_CYCLES;
  timedout = false;
  while (current_summed < number_to_sum)
  {
    curtime = CPU_CYCLES;
    if (((unsigned int)(curtime - inittime)) > timeout)
    {
      timedout = true;
      break;
    }
  }
  if (timedout)
  {
    Serial.println("ACQ: Timeout waiting for acquisition");
    return -1;
  }
  return 0;
}

int setup_frequency_acquire(unsigned int frequency)
{
  if ((frequency < VNA_MIN_FREQ) || (frequency > VNA_MAX_FREQ))
    return -1;

  if (frequency < VNA_FREQ_3X)
  {
    si5351.set_freq(frequency * SI5351_FREQ_MULT, SI5351_CLK0);
    si5351.set_freq((frequency + VNA_NOMINAL_1X_IF_FREQ) * SI5351_FREQ_MULT, SI5351_CLK1);
    numphases = 4;
  } else
  {
    frequency = frequency / 3;
    si5351.set_freq(frequency * SI5351_FREQ_MULT, SI5351_CLK0);
    si5351.set_freq((frequency + VNA_NOMINAL_3X_IF_FREQ) * SI5351_FREQ_MULT, SI5351_CLK1);
    numphases = 12;
  }
  switch (vna_state.atten)
  {
    case 0:  digitalWrite(ATTEN_PIN, frequency < VNA_AUTO_ATTEN_FREQ ? HIGH : LOW);
      break;
    case 1:  digitalWrite(ATTEN_PIN, LOW);
      break;
    case 2:  digitalWrite(ATTEN_PIN, HIGH);
      break;
  }
  return 0;
}

typedef int (*vna_acquire_dataset_operation)(
  unsigned int n,
  unsigned int total,
  unsigned int freq,
  int volti,
  int voltq,
  int curi,
  int curq,
  int cur2i,
  int cur2q,
  void *v);

int vna_display_dataset_operation(
  unsigned int n,
  unsigned int total,
  unsigned int freq,
  int volti,
  int voltq,
  int curi,
  int curq,
  int cur2i,
  int cur2q,
  void *va)
{
  if (vna_state.csv)
  {
    Serial.print(freq);
    Serial.print(",");
    Serial.print(volti);
    Serial.print(",");
    Serial.print(voltq);
    Serial.print(",");
    Serial.print(curi);
    Serial.print(",");
    Serial.print(curq);
    Serial.print(",");
    Serial.print(cur2i);
    Serial.print(",");
    Serial.println(cur2q);
  } else
  {
    Serial.print("Freq ");
    Serial.print(freq);
    Serial.print(": Volt (");
    Serial.print(volti);
    Serial.print(",");
    Serial.print(voltq);
    Serial.print(") Cur1 (");
    Serial.print(curi);
    Serial.print(",");
    Serial.print(curq);
    Serial.print(") Cur2 (");
    Serial.print(cur2i);
    Serial.print(",");
    Serial.print(cur2q);
    Serial.println(")");
  }
}

int vna_acquire_dataset(vna_acquisition_state *vs, vna_acquire_dataset_operation vado, void *v);

int vna_acquire_dataset(vna_acquisition_state *vs, vna_acquire_dataset_operation vado, void *v)
{
  int n;
  unsigned int freqstep = (vs->endfreq - vs->startfreq) / vs->nfreqs;
  for (n = 0; n < vs->nfreqs; n++)
  {
    unsigned int freq = vs->startfreq + freqstep * n;
    setup_frequency_acquire(freq);
    if (n == 0)
    {
      if (acquire_sample(12, vs->timeout) < 0)
      {
        Serial.println("Acquisition aborted");
        return -1;
      }
    }
    if (acquire_sample(vs->num_averages, vs->timeout) < 0)
    {
      Serial.println("Acquisition aborted");
      return -1;
    }
    vado(n, vs->nfreqs, freq, sampVOLTI, sampVOLTQ, sampCURI, sampCURQ, sampCUR2I, sampCUR2Q, v);
    if (Serial.available())
    {
      if (Serial.read() == '!')
      {
        Serial.println("Acquisition cancelled");
        return -1;
      }
    }
  }
  return 0;
}

int averages_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n;
  vna_acquisition_state *vs = &vna_state;
  vs->num_averages = tp[0].ti.i;
  vs->timeout = tp[1].ti.i;
  Serial.print("Averages=");
  Serial.println(vs->num_averages);
  Serial.print("Timeout=");
  Serial.println(vs->timeout);
}

int csv_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n;
  vna_acquisition_state *vs = &vna_state;
  vs->csv = tp[0].ti.i;
  Serial.print("CSV=");
  Serial.println(vs->csv);
}

int debug_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n;
  vna_acquisition_state *vs = &vna_state;
  setDebugMsgMode(tp[0].ti.i);
  Serial.print("DEBUG=");
  Serial.println(debugmsg_state);
}

const char *atten_strings[3] = { "0 Auto", "1 Off", "2 On" };

int atten_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n = tp[0].ti.i;
  vna_acquisition_state *vs = &vna_state;

  if ((n < 0) || (n > 2))
  {
    Serial.println("Invalid attenuator setting");
    return 0;
  }
  vs->atten = n;
  Serial.print("atten=");
  Serial.println(atten_strings[vs->atten]);
  return 1;
}

int doacq_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n;
  vna_acquisition_state *vs = &vna_state;
  vna_acquire_dataset(vs, vna_display_dataset_operation, NULL);
}

int setacq_cmd(int args, tinycl_parameter* tp, void *v)
{
  vna_acquisition_state *vs = &vna_state;
  unsigned int nfreqs = tp[0].ti.i;
  unsigned int startfreq = tp[1].ti.i;
  unsigned int endfreq = tp[2].ti.i;
  if ((nfreqs >= 1) && (nfreqs <= VNA_MAX_FREQS)) vs->nfreqs = nfreqs;
  if ((startfreq >= VNA_MIN_FREQ) && (endfreq > startfreq) && (endfreq <= VNA_MAX_FREQ))
  {
    vs->startfreq = startfreq;
    vs->endfreq = endfreq;
  }
  Serial.print("Start Frequency = ");
  Serial.println(vs->startfreq);
  Serial.print("End Frequency = ");
  Serial.println(vs->endfreq);
  Serial.print("Number Frequencies = ");
  Serial.println(vs->nfreqs);
  if (vna_state.calib_state != VNA_NO_CALIB)
  {
    Serial.println("Calibration state reset");
    reset_calib_state();
  }
}

int imacq_cmd(int args, tinycl_parameter* tp, void *v)
{
  vna_acquisition_state vs;
  vs.nfreqs = tp[0].ti.i;
  vs.startfreq = tp[1].ti.i;
  vs.endfreq = tp[2].ti.i;
  vs.timeout = vna_state.timeout;
  vs.num_averages = vna_state.num_averages;
  if (vs.nfreqs < 1)
  {
    Serial.println("Invalid number of frequencies");
    return -1;
  }
  if ((vs.startfreq < VNA_MIN_FREQ) || (vs.endfreq < vs.startfreq) && (vs.endfreq > VNA_MAX_FREQ))
  {
    Serial.println("Invalid maximum or minimum frequency");
    return -1;
  }
  vna_acquire_dataset(&vs, vna_display_dataset_operation, NULL);
  return 0;
}

int vna_open_dataset_operation(
  unsigned int n,
  unsigned int total,
  unsigned int freq,
  int volti,
  int voltq,
  int curi,
  int curq,
  int cur2i,
  int cur2q,
  void *va)
{
  vna_calib_oneport *v1pt = (vna_calib_oneport *)va;
  v1pt[n].zo = Complex((float)volti, (float)voltq) / Complex((float)curi, (float)curq);
  DEBUGMSG(".freq=%u zo=%d+i %d", freq, (int)(1000.0f * v1pt[n].zo.real), (int)(1000.0f * v1pt[n].zo.imag));
}

int vna_short_dataset_operation(
  unsigned int n,
  unsigned int total,
  unsigned int freq,
  int volti,
  int voltq,
  int curi,
  int curq,
  int cur2i,
  int cur2q,
  void *va)
{
  vna_calib_oneport *v1pt = (vna_calib_oneport *)va;
  v1pt[n].zs = Complex((float)volti, (float)voltq) / Complex((float)curi, (float)curq);
  vna_calib[n].s2 = Complex((float)cur2i,(float)cur2q) / ((float) vna_state.num_averages);
  DEBUGMSG(".freq=%u zs=%d+i %d", freq, (int)(1000.0f * v1pt[n].zs.real), (int)(1000.0f * v1pt[n].zs.imag));
}

int vna_load_dataset_operation(
  unsigned int n,
  unsigned int total,
  unsigned int freq,
  int volti,
  int voltq,
  int curi,
  int curq,
  int cur2i,
  int cur2q,
  void *va)
{
  vna_calib_oneport *v1pt = (vna_calib_oneport *)va;
  v1pt[n].zt = Complex((float)volti, (float)voltq) / Complex((float)curi, (float)curq);
  DEBUGMSG(".freq=%u zt=%d+i %d", freq, (int)(1000.0f * v1pt[n].zt.real), (int)(1000.0f * v1pt[n].zt.imag));
}

int opencalib_cmd(int args, tinycl_parameter* tp, void *v)
{
  vna_acquisition_state *vs = &vna_state;
  if (vna_1pt != NULL) free(vna_1pt);
  if ((vna_1pt = (vna_calib_oneport *) malloc(sizeof(vna_calib_oneport) * vs->nfreqs)) == NULL)
  {
    Serial.println("Unable to allocate memory for 1 port calib aborted");
    vna_state.calib_state = VNA_NO_CALIB;
    return -1;
  }
  vna_acquire_dataset(vs, vna_open_dataset_operation, (void *)vna_1pt);
  Serial.println("\r\nOpen calibration successful");
  vna_state.calib_state = VNA_OPEN;
  return 0;
}

int shortcalib_cmd(int args, tinycl_parameter* tp, void *v)
{
  vna_acquisition_state *vs = &vna_state;
  if (vna_state.calib_state != VNA_OPEN)
  {
    Serial.println("Short must be after OPEN calibration");
    return -1;
  }
  if (vna_1pt == NULL)
  {
    Serial.println("Memory for calibration unallocated!");
    return -1;
  }
  vna_acquire_dataset(vs, vna_short_dataset_operation, (void *)vna_1pt);
  Serial.println("\r\nShort calibration successful");
  vna_state.calib_state = VNA_SHORT;
  return 0;
}

int loadcalib_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n;
  vna_acquisition_state *vs = &vna_state;
  if (vna_state.calib_state != VNA_SHORT)
  {
    Serial.println("Load must be after SHORT calibration");
    return -1;
  }
  if (vna_1pt == NULL)
  {
    Serial.println("Memory for calibration unallocated!");
    return -1;
  }
  vs->char_impedance = tp[0].ti.i;
  float z0 = vs->char_impedance;
  vna_acquire_dataset(vs, vna_load_dataset_operation, (void *)vna_1pt);
  for (n = 0; n < vs->nfreqs; n++)
  {
    vna_calib_freq_parm *vc = &vna_calib[n];
    vc->b = -vna_1pt[n].zs;
    vc->c = (vna_1pt[n].zs - vna_1pt[n].zt) / ((vna_1pt[n].zo - vna_1pt[n].zt) * z0);
    vc->d = (-vna_1pt[n].zo) * vc->c;
  }
  Serial.println("\r\nOne port calibration successful");
  vna_state.calib_state = VNA_VALID_CALIB_1PT;
  free(vna_1pt);
  vna_1pt = NULL;
  return 0;
}

int vna_twocalib_dataset_operation(
  unsigned int n,
  unsigned int total,
  unsigned int freq,
  int volti,
  int voltq,
  int curi,
  int curq,
  int cur2i,
  int cur2q,
  void *va)
{
  Complex voltpt((float)volti, (float)voltq);
  Complex curpt((float)curi, (float)curq);
  Complex cur2pt((float)cur2i, (float)cur2q);
  vna_calib_freq_parm *vc = &vna_calib[n];
  Complex vn = voltpt + curpt * vc->b;
  Complex in = curpt * vc->d + voltpt * vc->c;
  cur2pt = cur2pt - vc->s2*((float)vna_state.num_averages);
  vc->z2 = vn / cur2pt;
  vc->i2 = in / cur2pt;
  DEBUGMSG(".freq=%u z2=%d+i %d i2=%d+i %d", freq, (int)(1000.0f * vc->z2.real), (int)(1000.0f * vc->z2.imag), (int)(1000.0f * vc->i2.real), (int)(1000.0f * vc->i2.imag));
}

int twocalib_cmd(int args, tinycl_parameter* tp, void *v)
{
  vna_acquisition_state *vs = &vna_state;
  if ((vna_state.calib_state != VNA_VALID_CALIB_1PT) && (vna_state.calib_state != VNA_VALID_CALIB_2PT))
  {
    Serial.println("Two Port Must Have Valid One Port Cal");
    return -1;
  }
  vna_acquire_dataset(vs, vna_twocalib_dataset_operation, NULL);
  Serial.println("\r\nTwo port calibration successful");
  vna_state.calib_state = VNA_VALID_CALIB_2PT;
  return 0;
}

void printfloat(float f)
{
  unsigned int i;
  if (f < 0.0f)
  {
    Serial.print("-");
    f = -f;
  }
  Serial.print((int)f);
  Serial.print(".");
  i = ((unsigned int)floor((f - floor(f)) * 10000.0f));
  if (i < 10) Serial.print("000");
  else if (i < 100) Serial.print("00");
  else if (i < 1000) Serial.print("0");
  Serial.print(i);
}

int vna_display_acq_operation(
  unsigned int n,
  unsigned int total,
  unsigned int freq,
  int volti,
  int voltq,
  int curi,
  int curq,
  int cur2i,
  int cur2q,
  void *va)
{
  Complex voltpt((float)volti, (float)voltq);
  Complex curpt((float)curi, (float)curq);
  vna_calib_freq_parm *vc = &vna_calib[n];
  Complex vn = voltpt + curpt * vc->b;
  Complex in = curpt * vc->d + voltpt * vc->c;
  Complex imp = vn / in;
  Complex zthru;
  if (vna_state.calib_state == VNA_VALID_CALIB_2PT)
  {
    Complex curpt2((float)cur2i, (float)cur2q);
    curpt2 = curpt2 - vc->s2 * ((float)vna_state.num_averages);
    zthru = (vn - curpt2 * vc->z2) / (curpt2 * vc->i2);
    //float z0 = (float)vna_state.char_impedance;
    //Complex s21 = curpt2 * (vc->z2 + vc->i2 * z0) / (vn + in * z0);
    //zthru = ((s21-1.0f)/s21)*(-2.0f*z0);
  }
  if (vna_state.csv)
  {
    Serial.print(freq);
    Serial.print(",");
    printfloat(imp.real);
    Serial.print(",");
    printfloat(-imp.imag);
    if (vna_state.calib_state == VNA_VALID_CALIB_2PT)
    {
      Serial.print(",");
      printfloat(zthru.real);
      Serial.print(",");
      printfloat(-zthru.imag);
    }
    Serial.println("");
  } else
  {
    Serial.print("Freq ");
    Serial.print(freq);
    Serial.print(": Z (");
    printfloat(imp.real);
    Serial.print(",");
    printfloat(-imp.imag);
    if (vna_state.calib_state == VNA_VALID_CALIB_2PT)
    {
      Serial.print(") ZT (");
      printfloat(zthru.real);
      Serial.print(",");
      printfloat(-zthru.imag);
    }
    Serial.println(")");
  }
}

int acq_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n;
  vna_acquisition_state *vs = &vna_state;
  if ((vna_state.calib_state != VNA_VALID_CALIB_1PT) && (vna_state.calib_state != VNA_VALID_CALIB_2PT))
  {
    Serial.println("Can only be performed after 1 or 2 port calibration");
    return -1;
  }
  vna_acquire_dataset(vs, vna_display_acq_operation, NULL);
}

int vna_display_sparm_operation(
  unsigned int n,
  unsigned int total,
  unsigned int freq,
  int volti,
  int voltq,
  int curi,
  int curq,
  int cur2i,
  int cur2q,
  void *va)
{
  Complex voltpt((float)volti, (float)voltq);
  Complex curpt((float)curi, (float)curq);
  vna_calib_freq_parm *vc = &vna_calib[n];
  Complex vn = voltpt + curpt * vc->b;
  Complex in = curpt * vc->d + voltpt * vc->c;
  Complex imp = vn / in;
  float z0 = (float)vna_state.char_impedance;
  Complex s11 = (imp - z0) / (imp + z0);
  float s11db = (10.0f / logf(10.0f)) * logf(s11.absq());
  float s11deg = RAD2DEG(s11.arg());
  Complex s21;
  float s21db, s21deg;
  if (vna_state.calib_state == VNA_VALID_CALIB_2PT)
  {
    Complex curpt2((float)cur2i, (float)cur2q);
    curpt2 = curpt2 - vc->s2 * ((float)vna_state.num_averages);
    //s21 = (curpt2 * vc->z2) / vn;
    s21 = curpt2 * (vc->z2 + vc->i2 * z0) / (vn + in * z0);
    s21db = (10.0f / logf(10.0f)) * logf(s21.absq());
    s21deg = RAD2DEG(s21.arg());
  }
  if (vna_state.csv)
  {
    Serial.print(freq);
    Serial.print(",");
    printfloat(s11db);
    Serial.print(",");
    printfloat(-s11deg);
    if (vna_state.calib_state == VNA_VALID_CALIB_2PT)
    {
      Serial.print(",");
      printfloat(s21db);
      Serial.print(",");
      printfloat(-s21deg);
    }
    Serial.println("");
  } else
  {
    Serial.print("Freq ");
    Serial.print(freq);
    Serial.print(": S11 (");
    printfloat(s11db);
    Serial.print(",");
    printfloat(-s11deg);
    if (vna_state.calib_state == VNA_VALID_CALIB_2PT)
    {
      Serial.print(") S21 (");
      printfloat(s21db);
      Serial.print(",");
      printfloat(-s21deg);
    }
    Serial.println(")");
  }
}

int sparm_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n;
  vna_acquisition_state *vs = &vna_state;
  if ((vna_state.calib_state != VNA_VALID_CALIB_1PT) && (vna_state.calib_state != VNA_VALID_CALIB_2PT))
  {
    Serial.println("Can only be performed after 1 or 2 port calibration");
    return -1;
  }
  vna_acquire_dataset(vs, vna_display_sparm_operation, NULL);
}

unsigned int flash_pages[] = { 0x08018000u, 0x08019000u, 0x0801A000u, 0x0801B000u, 0x0801C000u, 0x0801D000u, 0x0801E000u, 0x0801F000u };
#define NUM_FLASH_PAGES ((sizeof(flash_pages)/sizeof(void *)))

int writecal_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n = tp[0].ti.i;
  void *vp[3];
  int b[3];

  if ((vna_state.calib_state != VNA_VALID_CALIB_1PT) && (vna_state.calib_state != VNA_VALID_CALIB_2PT))
  {
    Serial.println("There is no calibration to write");
    return -1;
  }
  if ((n < 1) || (n > NUM_FLASH_PAGES))
  {
    Serial.println("Invalid calibration number");
    return -1;
  }
  Serial.print("Writing calibration ");
  Serial.print(n);

  vp[0] = (void *)&flash_header;
  b[0] = sizeof(flash_header);
  vp[1] = &vna_state;
  b[1] = sizeof(vna_state);
  vp[2] = vna_calib;
  b[2] = sizeof(vna_calib);

  Serial.println(writeflashstruct((void *)flash_pages[n - 1], 3, vp, b) ? ": written" : ": failed");
  return 0;
}

int readcal_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n = tp[0].ti.i;
  vna_flash_header rd_flash_header;
  void *vp[3];
  int b[3];

  if ((n < 1) || (n > NUM_FLASH_PAGES))
  {
    Serial.println("Invalid calibration number");
    return -1;
  }

  vp[0] = &rd_flash_header;
  b[0] = sizeof(rd_flash_header);
  readflashstruct((void *)flash_pages[n - 1], 1, vp, b);

  if (!((rd_flash_header.flash_header_1 == flash_header.flash_header_1) && (rd_flash_header.flash_header_2 == flash_header.flash_header_2)))
  {
    Serial.println("No calibration stored");
    return 0;
  }

  vp[0] = NULL;
  b[0] = sizeof(flash_header);
  vp[1] = &vna_state;
  b[1] = sizeof(vna_state);
  vp[2] = vna_calib;
  b[2] = sizeof(vna_calib);
  readflashstruct((void *)flash_pages[n - 1], 3, vp, b);

  Serial.print("Read calibration ");
  Serial.println(n);
  return 0;
}

int listcal_cmd(int args, tinycl_parameter* tp, void *v)
{
  int n;
  vna_flash_header rd_flash_header;
  vna_acquisition_state rd_vna_state;
  void *vp[2];
  int b[2];
  vp[0] = &rd_flash_header;
  b[0] = sizeof(rd_flash_header);
  vp[1] = &rd_vna_state;
  b[1] = sizeof(rd_vna_state);
  Serial.println("Calibrations in Flash:");
  for (n = 0; n < NUM_FLASH_PAGES; n++)
  {
    if (readflashstruct((void *)flash_pages[n], 2, vp, b))
    {
      if ((rd_flash_header.flash_header_1 == flash_header.flash_header_1) && (rd_flash_header.flash_header_2 == flash_header.flash_header_2))
      {
        Serial.print(n + 1);
        Serial.print(": Start ");
        Serial.print(rd_vna_state.startfreq);
        Serial.print(" End ");
        Serial.print(rd_vna_state.endfreq);
        Serial.print(" # ");
        Serial.print(rd_vna_state.nfreqs);
        Serial.print(" Impedance ");
        Serial.print(rd_vna_state.char_impedance);
        Serial.print(" Avgs ");
        Serial.print(rd_vna_state.num_averages);
        if (rd_vna_state.calib_state == VNA_VALID_CALIB_1PT)
          Serial.println(" 1 Port");
        else if (rd_vna_state.calib_state == VNA_VALID_CALIB_2PT)
          Serial.println(" 2 Port");
        else Serial.println(" No Cal");
      }
    }
  }
  Serial.println("-----");
}

const tinycl_command tcmds[] =
{
  { "CSV", "Set Comma Separated Values Mode", csv_cmd, TINYCL_PARM_INT, TINYCL_PARM_END },
  { "ATTEN", "Attenuator Setting", atten_cmd, TINYCL_PARM_INT, TINYCL_PARM_END },
  { "DEBUG", "Debug Messages", debug_cmd, TINYCL_PARM_INT, TINYCL_PARM_END },
  { "AVERAGES", "Set Averages", averages_cmd, TINYCL_PARM_INT, TINYCL_PARM_INT, TINYCL_PARM_END },
  { "IMACQ", "Immediate Acquisition", imacq_cmd, TINYCL_PARM_INT, TINYCL_PARM_INT, TINYCL_PARM_INT },
  { "SETACQ", "Set Acquisition Parameters", setacq_cmd, TINYCL_PARM_INT, TINYCL_PARM_INT, TINYCL_PARM_INT, TINYCL_PARM_END },
  { "DOACQ", "Do Acquisition Parameters", doacq_cmd, TINYCL_PARM_END },
  { "ACQ", "Acquisition of Ref/Thru", acq_cmd, TINYCL_PARM_END },
  { "SPARM", "Acquisition of Ref/Thru S parameters", sparm_cmd, TINYCL_PARM_END },
  { "SHORT", "Short Calibration", shortcalib_cmd, TINYCL_PARM_END },
  { "OPEN", "Open Calibration", opencalib_cmd, TINYCL_PARM_END },
  { "LOAD", "Load Calibration", loadcalib_cmd, TINYCL_PARM_INT, TINYCL_PARM_END },
  { "TWOCAL", "Two Port Calibration", twocalib_cmd, TINYCL_PARM_END },
  { "THRU", "Also Two Port Calibration", twocalib_cmd, TINYCL_PARM_END },
  { "LISTCAL", "List Calibration States", listcal_cmd, TINYCL_PARM_END },
  { "WRITECAL", "Write Calibration State", writecal_cmd, TINYCL_PARM_INT, TINYCL_PARM_END },
  { "READCAL", "Read Calibration State", readcal_cmd, TINYCL_PARM_INT, TINYCL_PARM_END },
  { "HELP", "Display This Help", help_cmd, {TINYCL_PARM_END } },
};

int help_cmd(int args, tinycl_parameter *tp, void *v)
{
  tinycl_print_commands(sizeof(tcmds) / sizeof(tinycl_command), tcmds);
}

void loop(void)
{
  if (tinycl_task(sizeof(tcmds) / sizeof(tinycl_command), tcmds, NULL))
    Serial.print("> ");
}