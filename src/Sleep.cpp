#include <Arduino.h>
#include <avr/sleep.h>
#include <util/atomic.h>
#include "Sleep.h"


// TODO:
// for now this is pretty broken, we need to
// verify the check in `loop` and handle the
// callback.

static volatile bool timer_match;
static volatile bool mesh_timer_match;

Duration Sleep::lastOverflow = {0, 0};
Duration Sleep::totalSleep = {0, 0};
Duration meshSleep = {0, 0};
uint32_t meshSleepStart = 0;
void (*sleepCallback)(uint32_t elapsed);
bool canSleep = false;


// Returns the time mesh slept since startup
const Duration& Sleep::meshsleeptime() {
  return meshSleep;
}

ISR(SCNT_CMP3_vect) {
  timer_match = true;
}

ISR(SCNT_CMP2_vect) {
  mesh_timer_match = true;
}

ISR(SCNT_OVFL_vect) {
  Sleep::lastOverflow += (1LL << 32) * Sleep::US_PER_TICK;
}

/* Do nothing, just meant to wake up the Scout. We would want to declare
 * this ISR weak, so a custom sketch can still use it - but it seems
 * doing so prevents the empty ISR from being used (instead falling bad
 * to the also weak __bad_interrupt). */
EMPTY_INTERRUPT(PCINT0_vect);

uint32_t Sleep::read_sccnt() {
  // Read LL first, that will freeze the other registers for reading
  uint32_t sccnt = SCCNTLL;
  sccnt |= (uint32_t)SCCNTLH << 8;
  sccnt |= (uint32_t)SCCNTHL << 16;
  sccnt |= (uint32_t)SCCNTHH << 24;
  return sccnt;
}

uint32_t Sleep::read_scocr3() {
  // Read LL first, that will freeze the other registers for reading
  uint32_t sccnt = SCOCR3LL;
  sccnt |= (uint32_t)SCOCR3LH << 8;
  sccnt |= (uint32_t)SCOCR3HL << 16;
  sccnt |= (uint32_t)SCOCR3HH << 24;
  return sccnt;
}

void Sleep::write_scocr3(uint32_t val) {
  // Write LL last, that will update the entire register atomically
  SCOCR3HH = val >> 24;
  SCOCR3HL = val >> 16;
  SCOCR3LH = val >> 8;
  SCOCR3LL = val;
}

void Sleep::write_scocr2(uint32_t val) {
  // Write LL last, that will update the entire register atomically
  SCOCR2HH = val >> 24;
  SCOCR2HL = val >> 16;
  SCOCR2LH = val >> 8;
  SCOCR2LL = val;
}

void Sleep::setup() {
  sleepPending = false;

  // Enable asynchronous mode for timer2. This is required to start the
  // 32kiHz crystal at all, so we can use it for the symbol counter. See
  // http://www.avrfreaks.net/index.php?name=PNphpBB2&file=viewtopic&t=142962
  ASSR |= (1 << AS2);

  // Timer2 is used for PWM on the blue led on the Pinoccio scout, by
  // default using 16Mhz/64=250kHz. We set the prescaler to 1 (32kiHz)
  // to come as close to that as possible. This results in a PWM
  // frequency of 32k/256 = 128Hz, which should still be sufficient for
  // a LED.
  TCCR2B = (TCCR2B & ~((1<<CS22)|(1<<CS21))) | (1<<CS20);

  // Enable the symbol counter, using the external 32kHz crystal
  // SCCR1 is left at defaults, with CMP3 in absolute compare mode.
  SCCR0 |= (1 << SCEN) | (1 << SCCKSEL);

  // Enable the SCNT_OVFL interrupt for timekeeping
  SCIRQM |= (1 << IRQMOF);

  // Enable the pin change interrupt 0 (for PCINT0-7). Individual pins
  // remain disabled until we actually sleep.
  PCICR |= (1 << PCIE0);
}

void Sleep::loop() {
  if (mesh_timer_match) {
    uint32_t after = read_sccnt();
    meshSleep += (uint64_t)(after - meshSleepStart) * US_PER_TICK;
    NWK_WakeupReq();
    mesh_timer_match = false;
  }

  canSleep = !NWK_Busy();

  if (sleepPending) {
    uint32_t left = scheduledTicksLeft();
    if (left != 0 && canSleep) {
      sleepPending = false;

      NWK_SleepReq();
      doSleep(true);
      NWK_WakeupReq();

      // Call the callback and clean up
      sleepCallback(left);
      sleepCallback = NULL;
    }
  }
}

void Sleep::scheduleSleep(uint32_t ms, void (*callback)(uint32_t elapsed)) {
  sleepCallback = callback;

  uint32_t ticks = msToTicks(ms);
  // Make sure we cannot "miss" the compare match if a low timeout is
  // passed (really only ms = 0, which is forbidden, but handle it
  // anyway).
  if (ticks < 2) ticks = 2;
  // Disable interrupts to prevent the counter passing the target before
  // we clear the IRQSCP3 flag (due to other interrupts happening)
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    // Schedule SCNT_CMP3 when the given counter is reached
    write_scocr3(read_sccnt() + ticks);

    // Clear any previously pending interrupt
    SCIRQS = (1 << IRQSCP3);
    timer_match = false;
  }

  sleepPending = true; // we want to sleep
}


// Sleep until the timer match interrupt fired. If interruptible is
// true, this can return before if some other interrupt wakes us up
// from sleep. If this happens, true is returned.
bool Sleep::sleepUntilMatch(bool interruptible) {
  while (true) {
    #ifdef sleep_bod_disable
    // On 256rfr2, BOD is automatically disabled in deep sleep, but
    // some other MCUs need explicit disabling. This should happen shortly
    // before actually sleeping. It's always automatically re-enabled.
    sleep_bod_disable();
    #endif
    sei();
    // AVR guarantees that the instruction after sei is executed, so
    // there is no race condition here
    sleep_cpu();
    // Immediately disable interrupts again, to ensure that
    // exactly one interrupt routine runs after wakeup, so
    // we prevent race conditions and can properly detect if
    // another interrupt than overflow occurred.
    cli();
    if (!timer_match && interruptible) {
      // We were woken up, but the overflow interrupt
      // didn't run, so another interrupt must have
      // triggered. Note that if the overflow
      // interrupt did trigger but not run yet, but also another
      // (lower priority) interrupt occured, its flag will
      // remain set and it will immediately wake us up
      // on the next sleep attempt.
      return false;
    }
    // See if overflow happened. Also check the IRQSCP3 flag,
    // for the case where the overflow happens together with
    // another (higher priority) interrupt.
    if (timer_match || SCIRQS & (1 << IRQSCP3)) {
      SCIRQS = (1 << IRQSCP3);
      timer_match = true;
      return true;
    }
  }
}

uint32_t Sleep::scheduledTicksLeft() {
  uint32_t left = read_scocr3() - read_sccnt();

  // If a compare match has occured, we're past the end of sleep already.
  // We check this _after_ grabbing the counter value above, to prevent
  // a race condition where the counter goes past the compare value
  // after checking for the timer_match flag. We check both the
  // interrupt flag and the timer_match flag, to handle both before
  // and after sleeping (since before sleeping, the IRQ is disabled, but
  // during sleep the wakeup clears the flag).
  if ((SCIRQS & (1 << IRQSCP3)) || timer_match)
    return 0;
  return left;
}

// TODO take a few ticks off the schedule as it takes us some amount of
// time to service the interrupt, mark the flag, get around to the loop
// and wake the radio
void Sleep::sleepRadio(uint32_t ms) {

  uint32_t ticks = msToTicks(ms);

  // Make sure we cannot "miss" the compare match if a low timeout is
  // passed (really only ms = 0, which is forbidden, but handle it
  // anyway).
  if (ticks < 2) ticks = 2;
  // Disable interrupts to prevent the counter passing the target before
  // we clear the IRQSCP3 flag (due to other interrupts happening)
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    // Schedule SCNT_CMP2 when the given counter is reached
    write_scocr2(read_sccnt() + ticks);

    // Clear any previously pending interrupt
    SCIRQS = (1 << IRQSCP2);

    // Enable the SCNT_CMP2 interrupt to wake us from sleep
    SCIRQM |= (1 << IRQMCP2);
  }

  meshSleepStart = read_sccnt();

  while (NWK_Busy()) {}

  NWK_SleepReq();
}

void Sleep::doSleep(bool interruptible) {

  // Disable Analog comparator
  uint8_t acsr = ACSR;
  ACSR = (1 << ACD);

  // Disable ADC
  uint8_t adcsra = ADCSRA;
  ADCSRA &= ~(1 << ADEN);

  // Disable the TX side of UART0. If the 16u2 it connects to is
  // powered off, it offers a path to ground, so keeping the pin
  // enabled and high (TTL idle) wastes around 1mA of current. To
  // detect if the 16u2 is powered on, see if it pulls the RX pin
  // high (not 100% reliable, but worst case we'll have extra
  // power usage or some serial garbage).
  // Note that this only takes effect after finishing the current byte,
  // so if transmission is still going on when we sleep, this doesn't
  // actually help.
  // An alternative would be to enable the pullup on the pin disable TX
  // unconditionally, which keeps the line high. However, this still
  // wastes 10μA (nearly doubling the minimum power usage).
  uint8_t ucsr0b = UCSR0B;
  if (UCSR0B & (1 << TXEN0) && !digitalRead(RX0))
    UCSR0B &= ~(1 << TXEN0);

  // Power save mode disables the main clock, but keeps the
  // external clock enabled for timer2 and symbol counter
  set_sleep_mode(SLEEP_MODE_PWR_SAVE);
  sleep_enable();

  cli();

  // Stop timer2, otherwise it will keep running in power-save mode
  uint8_t tccr2b = TCCR2B;
  TCCR2B = 0;

  uint8_t eimsk = EIMSK;
  uint8_t pcmsk0 = PCMSK0;

  // Clear any pending PCINT0 flag
  PCIFR |= (1 << PCIF0);

  // Check to see if we haven't passed the until time yet. Due to
  // wraparound, we convert to an integer, but this effectively halves
  // the maximum sleep duration (if until is > 2^31 from from now, it'll
  // look like the same as if until is in the past).
  if (!(SCIRQS & (1 << IRQSCP3))) {
    // Enable the SCNT_CMP3 interrupt to wake us from sleep
    SCIRQM |= (1 << IRQMCP3);

    uint32_t before = read_sccnt();
    sleepUntilMatch(interruptible);
    uint32_t after = read_sccnt();
    totalSleep += (uint64_t)(after - before) * US_PER_TICK;

    // Disable the SCNT_CMP3 interrupt again
    SCIRQM &= ~(1 << IRQMCP3);
  }

  sleep_disable();

  PCMSK0 = pcmsk0;
  // Instead of calling detachInterrupt a dozen times, just restore the
  // original external interrupt mask
  EIMSK = eimsk;

  sei();

  // Restart timer2
  TCCR2B = tccr2b;

  // Restore other settings
  UCSR0B = ucsr0b;
  ACSR = acsr;
  ADCSRA = adcsra;
  while (!(ADCSRB & (1 << AVDDOK))) /* nothing */;
}

