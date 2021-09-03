#include "targets.h"
#include "common.h"
#include "LowPassFilter.h"

#if defined(Regulatory_Domain_AU_915) || defined(Regulatory_Domain_EU_868) || defined(Regulatory_Domain_IN_866) || defined(Regulatory_Domain_FCC_915) || defined(Regulatory_Domain_AU_433) || defined(Regulatory_Domain_EU_433)
#include "SX127xDriver.h"
SX127xDriver Radio;
#elif defined(Regulatory_Domain_ISM_2400)
#include "SX1280Driver.h"
SX1280Driver Radio;
#else
#error "Radio configuration is not valid!"
#endif

#ifdef PLATFORM_ESP8266
#include "ESP8266_WebUpdate.h"
#endif

#include "crc.h"
#include "CRSF.h"
#include "telemetry_protocol.h"
#include "telemetry.h"
#include "stubborn_sender.h"
#include "stubborn_receiver.h"

#include "FHSS.h"
#include "logging.h"
#include "OTA.h"
#include "msp.h"
#include "msptypes.h"
#include "hwTimer.h"
#include "PFD.h"
#include "LQCALC.h"
#include "elrs_eeprom.h"
#include "config.h"
#include "options.h"
#include "POWERMGNT.h"

#ifdef TARGET_RX_GHOST_ATTO_V1
uint8_t LEDfadeDiv;
uint8_t LEDfade;
bool LEDfadeDir;
uint32_t LEDWS2812LastUpdate;
#include "STM32F3_WS2812B_LED.h"
#endif

//// CONSTANTS ////
#define BUTTON_SAMPLE_INTERVAL 150
#define WEB_UPDATE_PRESS_INTERVAL 2000 // hold button for 2 sec to enable webupdate mode
#define BUTTON_RESET_INTERVAL 4000     //hold button for 4 sec to reboot RX
#define LED_INTERVAL_WEB_UPDATE 25
#define LED_INTERVAL_ERROR      100
#define LED_INTERVAL_DISCONNECTED 500
#define LED_INTERVAL_BIND_SHORT 100
#define LED_INTERVAL_BIND_LONG  1000
#define SEND_LINK_STATS_TO_FC_INTERVAL 50
#define DIVERSITY_ANTENNA_INTERVAL 30
#define DIVERSITY_ANTENNA_RSSI_TRIGGER 5
#define PACKET_TO_TOCK_SLACK 200 // Desired buffer time between Packet ISR and Tock ISR
///////////////////

uint8_t antenna = 0;    // which antenna is currently in use

hwTimer hwTimer;
PFD PFDloop;
GENERIC_CRC14 ota_crc(ELRS_CRC14_POLY);
ELRS_EEPROM eeprom;
RxConfig config;
Telemetry telemetry;

/* CRSF_TX_SERIAL is used by CRSF output */
#if defined(TARGET_RX_FM30_MINI)
    HardwareSerial CRSF_TX_SERIAL(USART2);
#else
    #define CRSF_TX_SERIAL Serial
#endif
CRSF crsf(CRSF_TX_SERIAL);

/* CRSF_RX_SERIAL is used by telemetry receiver and can be on a different peripheral */
#if defined(TARGET_RX_GHOST_ATTO_V1) /* !TARGET_RX_GHOST_ATTO_V1 */
    #define CRSF_RX_SERIAL CrsfRxSerial
    HardwareSerial CrsfRxSerial(USART1, HALF_DUPLEX_ENABLED);
#elif defined(TARGET_R9SLIMPLUS_RX) /* !TARGET_R9SLIMPLUS_RX */
    #define CRSF_RX_SERIAL CrsfRxSerial
    HardwareSerial CrsfRxSerial(USART3);
#elif defined(TARGET_RX_FM30_MINI)
    #define CRSF_RX_SERIAL CRSF_TX_SERIAL
#else
    #define CRSF_RX_SERIAL Serial
#endif

StubbornSender TelemetrySender(ELRS_TELEMETRY_MAX_PACKAGES);
static uint8_t telemetryBurstCount;
static uint8_t telemetryBurstMax;
// Maximum ms between LINK_STATISTICS packets for determining burst max
#define TELEM_MIN_LINK_INTERVAL 512U

StubbornReceiver MspReceiver(ELRS_MSP_MAX_PACKAGES);
uint8_t MspData[ELRS_MSP_BUFFER];

static uint8_t NextTelemetryType = ELRS_TELEMETRY_TYPE_LINK;
static bool telemBurstValid;
/// Filters ////////////////
LPF LPF_Offset(2); // Lenght is 4 elements
LPF LPF_OffsetDx(4); // Lenght is 16 elements

// LPF LPF_UplinkRSSI(5);
LPF LPF_UplinkRSSI0(5);  // track rssi per antenna
LPF LPF_UplinkRSSI1(5);


/// LQ Calculation //////////
LQCALC<100> LQCalc;
uint8_t uplinkLQ;

uint8_t scanIndex = RATE_DEFAULT;

int32_t RawOffset;
int32_t prevRawOffset;
int32_t Offset;
int32_t OffsetDx;
int32_t shiftPhaseBy;
uint8_t PhaseLockCounter;
RXtimerState_e RXtimerState;
uint32_t GotConnectionMillis = 0;
const uint32_t ConsiderConnGoodMillis = 1000; // minimum time before we can consider a connection to be 'good' - should be calculated from the sync interval (*4?) and at least one additional sync should have been received to confirm RXnonce and FHSSindex

// LED Blinking state
static bool LED = false;
static uint8_t LEDPulseCounter;
static uint32_t LEDLastUpdate;

//// Variables Relating to Button behaviour ////
bool buttonPrevValue = true; //default pullup
bool buttonDown = false;     //is the button current pressed down?
uint32_t buttonLastSampled = 0;
uint32_t buttonLastPressed = 0;

bool webUpdateMode = false;
bool disableWebServer = false;
///////////////////////////////////////////////

volatile uint8_t NonceRX = 0; // nonce that we THINK we are up to.

bool alreadyFHSS = false;
bool alreadyTLMresp = false;

uint32_t beginProcessing;
uint32_t doneProcessing;

//////////////////////////////////////////////////////////////

///////Variables for Telemetry and Link Quality///////////////
uint32_t LastValidPacket = 0;           //Time the last valid packet was recv
uint32_t LastSyncPacket = 0;            //Time the last valid packet was recv

uint32_t SendLinkStatstoFCintervalLastSent = 0;

int16_t RFnoiseFloor; //measurement of the current RF noise floor
#if defined(DEBUG_RX_SCOREBOARD)
static bool lastPacketCrcError;
#endif
///////////////////////////////////////////////////////////////

/// Variables for Sync Behaviour ////
uint32_t cycleInterval; // in ms
uint32_t RFmodeLastCycled = 0;
#define RFmodeCycleMultiplierSlow 10
uint8_t RFmodeCycleMultiplier;
bool LockRFmode = false;
///////////////////////////////////////

#if defined(BF_DEBUG_LINK_STATS)
// Debug vars
uint8_t debug1 = 0;
uint8_t debug2 = 0;
uint8_t debug3 = 0;
int8_t debug4 = 0;
///////////////////////////////////////
#endif

bool InBindingMode = false;

void reset_into_bootloader(void);
void EnterBindingMode();
void ExitBindingMode();
void UpdateModelMatch(uint8_t model);
void OnELRSBindMSP(uint8_t* packet);

static uint8_t minLqForChaos()
{
    // Determine the most number of CRC-passing packets we could receive on
    // a single channel out of 100 packets that fill the LQcalc span.
    // The LQ must be GREATER THAN this value, not >=
    // The amount of time we coexist on the same channel is
    // 100 divided by the total number of packets in a FHSS loop (rounded up)
    // and there would be 4x packets received each time it passes by so
    // FHSShopInterval * ceil(100 / FHSShopInterval * numfhss) or
    // FHSShopInterval * trunc((100 + (FHSShopInterval * numfhss) - 1) / (FHSShopInterval * numfhss))
    // With a interval of 4 this works out to: 2.4=4, FCC915=4, AU915=8, EU868=8, EU/AU433=36
    const uint32_t numfhss = FHSSgetChannelCount();
    const uint8_t interval = ExpressLRS_currAirRate_Modparams->FHSShopInterval;
    return interval * ((interval * numfhss + 99) / (interval * numfhss));
}

void ICACHE_RAM_ATTR getRFlinkInfo()
{
    int32_t rssiDBM0 = LPF_UplinkRSSI0.SmoothDataINT;
    int32_t rssiDBM1 = LPF_UplinkRSSI1.SmoothDataINT;

    switch (antenna) {
        case 0:
            rssiDBM0 = LPF_UplinkRSSI0.update(Radio.LastPacketRSSI);
            break;
        case 1:
            rssiDBM1 = LPF_UplinkRSSI1.update(Radio.LastPacketRSSI);
            break;
    }

    int32_t rssiDBM = (antenna == 0) ? rssiDBM0 : rssiDBM1;
    crsf.PackedRCdataOut.ch15 = UINT10_to_CRSF(map(constrain(rssiDBM, ExpressLRS_currAirRate_RFperfParams->RXsensitivity, -50),
                                               ExpressLRS_currAirRate_RFperfParams->RXsensitivity, -50, 0, 1023));
    crsf.PackedRCdataOut.ch14 = UINT10_to_CRSF(fmap(uplinkLQ, 0, 100, 0, 1023));

    if (rssiDBM0 > 0) rssiDBM0 = 0;
    if (rssiDBM1 > 0) rssiDBM1 = 0;

    // BetaFlight/iNav expect positive values for -dBm (e.g. -80dBm -> sent as 80)
    crsf.LinkStatistics.uplink_RSSI_1 = -rssiDBM0;
    crsf.LinkStatistics.active_antenna = antenna;
    crsf.LinkStatistics.uplink_SNR = Radio.LastPacketSNR;
    crsf.LinkStatistics.uplink_Link_quality = uplinkLQ;
    crsf.LinkStatistics.rf_Mode = (uint8_t)RATE_4HZ - (uint8_t)ExpressLRS_currAirRate_Modparams->enum_rate;
    //DBGLN(crsf.LinkStatistics.uplink_RSSI_1);
    #if defined(DEBUG_BF_LINK_STATS)
    crsf.LinkStatistics.downlink_RSSI = debug1;
    crsf.LinkStatistics.downlink_Link_quality = debug2;
    crsf.LinkStatistics.downlink_SNR = debug3;
    crsf.LinkStatistics.uplink_RSSI_2 = debug4;
    #else
    crsf.LinkStatistics.downlink_RSSI = 0;
    crsf.LinkStatistics.downlink_Link_quality = 0;
    crsf.LinkStatistics.downlink_SNR = 0;
    crsf.LinkStatistics.uplink_RSSI_2 = -rssiDBM1;
    #endif
}

void SetRFLinkRate(uint8_t index) // Set speed of RF link
{
    expresslrs_mod_settings_s *const ModParams = get_elrs_airRateConfig(index);
    expresslrs_rf_pref_params_s *const RFperf = get_elrs_RFperfParams(index);
    bool invertIQ = UID[5] & 0x01;

    // this function is also called to go back to the initial frequency when connection is lost
    // the timers freuqency offset does not need to be reset in that case
    if (ExpressLRS_currAirRate_Modparams->index != index)
    {
        // frequeny offset is in absolute numbers (us) and we are changing the interval.
        // best would be to calculate a value based on the actual rate difference between the old and new airrate
        hwTimer.resetFreqOffset();
    }

    hwTimer.updateInterval(ModParams->interval); // put this in the above condition too?
    Radio.Config(ModParams->bw, ModParams->sf, ModParams->cr, GetInitialFreq(), ModParams->PreambleLen, invertIQ, ModParams->PayloadLength);

    // Wait for (11/10) 110% of time it takes to cycle through all freqs in FHSS table (in ms)
    cycleInterval = ((uint32_t)11U * FHSSgetChannelCount() * ModParams->FHSShopInterval * ModParams->interval) / (10U * 1000U);

    ExpressLRS_currAirRate_Modparams = ModParams;
    ExpressLRS_currAirRate_RFperfParams = RFperf;
    ExpressLRS_nextAirRateIndex = index; // presumably we just handled this
    telemBurstValid = false;
}

bool ICACHE_RAM_ATTR HandleFHSS()
{
    uint8_t modresultFHSS = (NonceRX + 1) % ExpressLRS_currAirRate_Modparams->FHSShopInterval;

    if ((ExpressLRS_currAirRate_Modparams->FHSShopInterval == 0) || alreadyFHSS == true || InBindingMode || (modresultFHSS != 0) || (connectionState == disconnected))
    {
        return false;
    }

    alreadyFHSS = true;
    Radio.SetFrequencyReg(FHSSgetNextFreq());

    uint8_t modresultTLM = (NonceRX + 1) % (TLMratioEnumToValue(ExpressLRS_currAirRate_Modparams->TLMinterval));

    if (modresultTLM != 0 || ExpressLRS_currAirRate_Modparams->TLMinterval == TLM_RATIO_NO_TLM) // if we are about to send a tlm response don't bother going back to rx
    {
        Radio.RXnb();
    }
    return true;
}

bool ICACHE_RAM_ATTR HandleSendTelemetryResponse()
{
    uint8_t *data;
    uint8_t maxLength;
    uint8_t packageIndex;
    uint8_t modresult = (NonceRX + 1) % TLMratioEnumToValue(ExpressLRS_currAirRate_Modparams->TLMinterval);

    if ((connectionState == disconnected) || (ExpressLRS_currAirRate_Modparams->TLMinterval == TLM_RATIO_NO_TLM) || (alreadyTLMresp == true) || (modresult != 0))
    {
        return false; // don't bother sending tlm if disconnected or TLM is off
    }

    alreadyTLMresp = true;
    Radio.TXdataBuffer[0] = TLM_PACKET;

    if (NextTelemetryType == ELRS_TELEMETRY_TYPE_LINK || !TelemetrySender.IsActive())
    {
        Radio.TXdataBuffer[1] = ELRS_TELEMETRY_TYPE_LINK;
        // The value in linkstatistics is "positivized" (inverted polarity)
        // and must be inverted on the TX side. Positive values are used
        // so save a bit to encode which antenna is in use
        Radio.TXdataBuffer[2] = crsf.LinkStatistics.uplink_RSSI_1 | (antenna << 7);
        Radio.TXdataBuffer[3] = crsf.LinkStatistics.uplink_RSSI_2;
        Radio.TXdataBuffer[4] = crsf.LinkStatistics.uplink_SNR;
        Radio.TXdataBuffer[5] = crsf.LinkStatistics.uplink_Link_quality;
        Radio.TXdataBuffer[6] = MspReceiver.GetCurrentConfirm() ? 1 : 0;

        NextTelemetryType = ELRS_TELEMETRY_TYPE_DATA;
        telemetryBurstCount = 0;
    }
    else
    {
        if (telemetryBurstCount < telemetryBurstMax)
        {
            telemetryBurstCount++;
        }
        else
        {
            NextTelemetryType = ELRS_TELEMETRY_TYPE_LINK;
        }

        TelemetrySender.GetCurrentPayload(&packageIndex, &maxLength, &data);
        Radio.TXdataBuffer[1] = (packageIndex << ELRS_TELEMETRY_SHIFT) + ELRS_TELEMETRY_TYPE_DATA;
        Radio.TXdataBuffer[2] = maxLength > 0 ? *data : 0;
        Radio.TXdataBuffer[3] = maxLength >= 1 ? *(data + 1) : 0;
        Radio.TXdataBuffer[4] = maxLength >= 2 ? *(data + 2) : 0;
        Radio.TXdataBuffer[5] = maxLength >= 3 ? *(data + 3): 0;
        Radio.TXdataBuffer[6] = maxLength >= 4 ? *(data + 4): 0;
    }

    uint16_t crc = ota_crc.calc(Radio.TXdataBuffer, 7, CRCInitializer);
    Radio.TXdataBuffer[0] |= (crc >> 6) & 0b11111100;
    Radio.TXdataBuffer[7] = crc & 0xFF;

    Radio.TXnb();
    return true;
}

void ICACHE_RAM_ATTR HandleFreqCorr(bool value)
{
    //DBGVLN(FreqCorrection);
    if (!value)
    {
        if (FreqCorrection < FreqCorrectionMax)
        {
            FreqCorrection += 1; //min freq step is ~ 61hz but don't forget we use FREQ_HZ_TO_REG_VAL so the units here are not hz!
        }
        else
        {
            FreqCorrection = FreqCorrectionMax;
            FreqCorrection = 0; //reset because something went wrong
            DBGLN("Max +FreqCorrection reached!");
        }
    }
    else
    {
        if (FreqCorrection > FreqCorrectionMin)
        {
            FreqCorrection -= 1; //min freq step is ~ 61hz
        }
        else
        {
            FreqCorrection = FreqCorrectionMin;
            FreqCorrection = 0; //reset because something went wrong
            DBGLN("Max -FreqCorrection reached!");
        }
    }
}

/*
Phase Lock concept
1st Priority: Rough and quick phase adjustment during connection (re)establishing
2nd Priority: compensate for frequency missmatch by looking at "long term" change of OffsetDx 

RXtimerState = tim_disconnected: whenever a sync packet information does not line up with what
                we expect (means we have a major timer missmatch - phase mostly)
RXtimerState = tim_tentative: whenever Offset var (phase) is in a good range
RXtimerState = tim_locked: whenever OffsetDx (changes of timing) var is in a good range

Frequency Offset should be relatively constant for a given TX / RX combination (except temperature drift).
/**/

void ICACHE_RAM_ATTR updatePhaseLock()
{
    if (connectionState != disconnected && PFDloop.resultValid())
    {
        // absolute Offset (uSec) from last iteration
        RawOffset = PFDloop.getResult();
        // absolute Offset (uSec) LPFed (4 samples) --> compensate with phase shift
        Offset = LPF_Offset.update(RawOffset);
        // change in Offset per period (delta uSec) LPFed (16 samples)
        OffsetDx = LPF_OffsetDx.update(RawOffset - prevRawOffset);

        // only update timer frequency if we have an at least tentative timer lock
        //if (RXtimerState != tim_disconnected && LQCalc.currentIsSet()) // replaced by resultValid
        if (RXtimerState != tim_disconnected)
        {
            //limit rate of freq offset adjustment to every 8th phase lock iteration
            if (PhaseLockCounter % 8 == 0)
            {
                if (OffsetDx > 0)
                {
                    hwTimer.incFreqOffset();
                }
                else if (OffsetDx < 0)
                {
                    hwTimer.decFreqOffset();
                }
            }
        }

        if (RXtimerState == tim_disconnected)
        {
            // half raw offset for quick adjustment
            shiftPhaseBy = RawOffset >> 1; 
        }
        // limit rate of phase shifting to every 2nd phase lock iteration
        else if (PhaseLockCounter % 2 == 0)
        {
            // quarter LPF'ed offset; fits the width of the LPF (4) so no overshoot is possible
            shiftPhaseBy = Offset >> 2; 
        }
        else
        {
            shiftPhaseBy = 0;
        }

        hwTimer.phaseShift(shiftPhaseBy);
        // compensate for phase adjustments to eliminate impact on frequency correction
        prevRawOffset = RawOffset - shiftPhaseBy;
    }

    // if filtered or unfiltered Offset is exceeding this limit we are already in a bad place
    // by changing RXtimerState to disconnected the phase will be adjusted very quick
    // 500Hz -> dt=2000us: 500us filtered should be fine; 200us is the hard coded slack
    if (abs(Offset) > 500 || abs(RawOffset) > 600)
    {
        RXtimerState = tim_disconnected;
        DBGLN("Timer disconnected");
    }
    // now check if OffsetDx LPF is filled and how stable the Offset value is
    else if (abs(OffsetDx) <= 7 && PhaseLockCounter % 16 == 0)
    {
        RXtimerState = tim_locked; // precondition for connectionState=connected
        DBGLN("Timer locked");
    }
    // Offset if fine but we are not happy with the stability of the value
    else 
    {
        RXtimerState = tim_tentative;
        DBGLN("Timer tentative");
    }

    // get ready for next measurement; do this at the end of the function without any precondition
    PFDloop.reset();
    PhaseLockCounter++;

    DBGVLN("%d:%d:%d:%d:%d", Offset, RawOffset, OffsetDx, hwTimer.FreqOffset, uplinkLQ);
}

// this is 180 out of phase with the other callback, occurs mid of two packet receptions
void ICACHE_RAM_ATTR HWtimerCallbackTick() 
{
    updatePhaseLock();
    getRFlinkInfo();
    NonceRX++;

    // if (!alreadyTLMresp && !alreadyFHSS && !LQCalc.currentIsSet()) // packet timeout AND didn't DIDN'T just hop or send TLM
    // {
    //     Radio.RXnb(); // put the radio cleanly back into RX in case of garbage data
    // }

    // Save the LQ value before the inc() reduces it by 1
    uplinkLQ = LQCalc.getLQ();
    // Only advance the LQI period counter if we didn't send Telemetry this period
    if (!alreadyTLMresp)
        LQCalc.inc();

    alreadyTLMresp = false;
    alreadyFHSS = false;
    crsf.RXhandleUARTout();
}

//////////////////////////////////////////////////////////////
// flip to the other antenna
// no-op if GPIO_PIN_ANTENNA_SELECT not defined
static inline void switchAntenna()
{
#if defined(GPIO_PIN_ANTENNA_SELECT) && defined(USE_DIVERSITY)
    antenna = !antenna;
    digitalWrite(GPIO_PIN_ANTENNA_SELECT, antenna);
#endif
}

static void ICACHE_RAM_ATTR updateDiversity()
{
#if defined(GPIO_PIN_ANTENNA_SELECT) && defined(USE_DIVERSITY)
    static int32_t prevRSSI;        // saved rssi so that we can compare if switching made things better or worse
    static int32_t antennaLQDropTrigger;
    static int32_t antennaRSSIDropTrigger;
    int32_t rssi = (antenna == 0) ? LPF_UplinkRSSI0.SmoothDataINT : LPF_UplinkRSSI1.SmoothDataINT;
    int32_t otherRSSI = (antenna == 0) ? LPF_UplinkRSSI1.SmoothDataINT : LPF_UplinkRSSI0.SmoothDataINT;

    //if rssi dropped by the amount of DIVERSITY_ANTENNA_RSSI_TRIGGER
    if ((rssi < (prevRSSI - DIVERSITY_ANTENNA_RSSI_TRIGGER)) && antennaRSSIDropTrigger >= DIVERSITY_ANTENNA_INTERVAL)
    {
        switchAntenna();
        antennaLQDropTrigger = 1;
        antennaRSSIDropTrigger = 0;
    }
    else if (rssi > prevRSSI || antennaRSSIDropTrigger < DIVERSITY_ANTENNA_INTERVAL)
    {
        prevRSSI = rssi;
        antennaRSSIDropTrigger++;
    }

    // if we didn't get a packet switch the antenna
    if (!LQCalc.currentIsSet() && antennaLQDropTrigger == 0)
    {
        switchAntenna();
        antennaLQDropTrigger = 1;
        antennaRSSIDropTrigger = 0;
    }
    else if (antennaLQDropTrigger >= DIVERSITY_ANTENNA_INTERVAL)
    {
        // We switched antenna on the previous packet, so we now have relatively fresh rssi info for both antennas.
        // We can compare the rssi values and see if we made things better or worse when we switched
        if (rssi < otherRSSI)
        {
            // things got worse when we switched, so change back.
            switchAntenna();
            antennaLQDropTrigger = 1;
            antennaRSSIDropTrigger = 0;
        }
        else
        {
            // all good, we can stay on the current antenna. Clear the flag.
            antennaLQDropTrigger = 0;
        }
    }
    else if (antennaLQDropTrigger > 0)
    {
        antennaLQDropTrigger ++;
    }
#endif
}

void ICACHE_RAM_ATTR HWtimerCallbackTock()
{
    PFDloop.intEvent(micros()); // our internal osc just fired

    updateDiversity();
    bool didFHSS = HandleFHSS();
    bool tlmSent = HandleSendTelemetryResponse();

    #if !defined(Regulatory_Domain_ISM_2400)
    if (!didFHSS && !tlmSent && LQCalc.currentIsSet())
    {
        HandleFreqCorr(Radio.GetFrequencyErrorbool());      // Adjusts FreqCorrection for RX freq offset
        Radio.SetPPMoffsetReg(FreqCorrection);
    }
    #else
        (void)didFHSS;
        (void)tlmSent;
    #endif /* Regulatory_Domain_ISM_2400 */

    #if defined(DEBUG_RX_SCOREBOARD)
    static bool lastPacketWasTelemetry = false;
    if (!LQCalc.currentIsSet() && !lastPacketWasTelemetry)
        DBGW(lastPacketCrcError ? '.' : '_');
    lastPacketCrcError = false;
    lastPacketWasTelemetry = tlmSent;
    #endif
}

void LostConnection()
{
    DBGLN("lost conn fc=%d fo=%d", FreqCorrection, hwTimer.FreqOffset);

    RFmodeCycleMultiplier = 1;
    connectionStatePrev = connectionState;
    connectionState = disconnected; //set lost connection
    RXtimerState = tim_disconnected;
    //hwTimer.resetFreqOffset(); // done in SetRFLinkRate() when necessary
    FreqCorrection = 0;
    #if !defined(Regulatory_Domain_ISM_2400)
    Radio.SetPPMoffsetReg(0);
    #endif
    //Offset = 0; //value is calculated in UpdatePhaseLock() why manipulate it?
    //OffsetDx = 0; //value is calculated in UpdatePhaseLock() why manipulate it?
    //RawOffset = 0; //value is calculated in UpdatePhaseLock() why manipulate it?
    GotConnectionMillis = 0;
    
    uplinkLQ = 0;
    LQCalc.reset();
    
    Radio.LastPacketRSSI = ExpressLRS_currAirRate_RFperfParams->RXsensitivity;
    LPF_UplinkRSSI0.init(ExpressLRS_currAirRate_RFperfParams->RXsensitivity); // TEST
    LPF_UplinkRSSI1.init(ExpressLRS_currAirRate_RFperfParams->RXsensitivity); // TEST
    
    LPF_Offset.init(0); // necessary? (but does not hurt much)
    LPF_OffsetDx.init(0); // necessary? but does not hurt much)
    PhaseLockCounter = 0;
    alreadyTLMresp = false;
    alreadyFHSS = false;
    LED = false; // Make first LED cycle turn it on

    if (!InBindingMode)
    {
        while(micros() - PFDloop.getIntEventTime() > 250); // time it just after the tock()
        hwTimer.stop(); //stop timer to stop fhss and incrementing NonceRX
        SetRFLinkRate(ExpressLRS_nextAirRateIndex); // also sets to initialFreq
        Radio.RXnb();
    }

#ifdef GPIO_PIN_LED_GREEN
    digitalWrite(GPIO_PIN_LED_GREEN, LOW ^ GPIO_LED_GREEN_INVERTED);
#endif

#ifdef GPIO_PIN_LED_RED
    digitalWrite(GPIO_PIN_LED_RED, LOW ^ GPIO_LED_RED_INVERTED);
#endif

#ifdef GPIO_PIN_LED
    digitalWrite(GPIO_PIN_LED, LOW ^ GPIO_LED_RED_INVERTED); // turn off led
#endif
}

// called when a sync packet is received while we are in disconnected state and 
// whenever incoming sync data for Nonce and FHSS doesn't match the expected values
void ICACHE_RAM_ATTR TentativeConnection(unsigned long now)
{
    connectionStatePrev = connectionState;
    connectionState = tentative;
    RXtimerState = tim_disconnected;
    DBGLN("tentative conn");
    FreqCorrection = 0;
    LPF_Offset.init(0);
    LPF_OffsetDx.init(0);
    PhaseLockCounter = 0;
    //PFDloop.reset(); // TEST the above resets should be enough
    RFmodeLastCycled = now; // give another 3 sec for lock to occur

#if WS2812_LED_IS_USED
    uint8_t LEDcolor[3] = {0};
    LEDcolor[(2 - ExpressLRS_currAirRate_Modparams->index) % 3] = 50;
    WS281BsetLED(LEDcolor);
    LEDWS2812LastUpdate = now;
#endif

    // The caller MUST call hwTimer.resume(). It is not done here because
    // the timer ISR will fire immediately and preempt any other code
}

void GotConnection(unsigned long now)
{
    if (connectionState == connected)
    {
        return; // Already connected
    }

#ifdef LOCK_ON_FIRST_CONNECTION
    LockRFmode = true;
#endif

    connectionStatePrev = connectionState;
    connectionState = connected; //we got a packet, therefore no lost connection
    disableWebServer = true;
    GotConnectionMillis = now;

    DBGLN("got conn");

#if WS2812_LED_IS_USED
    uint8_t LEDcolor[3] = {0};
    LEDcolor[(2 - ExpressLRS_currAirRate_Modparams->index) % 3] = 255;
    WS281BsetLED(LEDcolor);
    LEDWS2812LastUpdate = now;
#endif

#ifdef GPIO_PIN_LED_GREEN
    digitalWrite(GPIO_PIN_LED_GREEN, HIGH ^ GPIO_LED_GREEN_INVERTED);
#endif

#ifdef GPIO_PIN_LED_RED
    digitalWrite(GPIO_PIN_LED_RED, HIGH ^ GPIO_LED_RED_INVERTED);
#endif

#ifdef GPIO_PIN_LED
    digitalWrite(GPIO_PIN_LED, HIGH ^ GPIO_LED_RED_INVERTED); // turn on led
#endif
}

void ICACHE_RAM_ATTR ProcessRFPacket()
{
    beginProcessing = micros();

    uint8_t type = Radio.RXdataBuffer[0] & 0b11;
    uint16_t inCRC = (((uint16_t)(Radio.RXdataBuffer[0] & 0b11111100)) << 6) | Radio.RXdataBuffer[7];

    // For smHybrid the CRC only has the packet type in byte 0
    // For smHybridWide the FHSS slot is added to the CRC in byte 0 except on SYNC packets
    if (type == SYNC_PACKET || OtaSwitchModeCurrent != smHybridWide)
    {
        Radio.RXdataBuffer[0] = type;
    }
    else
    {
        uint8_t NonceFHSSresult = NonceRX % ExpressLRS_currAirRate_Modparams->FHSShopInterval;
        Radio.RXdataBuffer[0] = type | (NonceFHSSresult << 2);
    }
    uint16_t calculatedCRC = ota_crc.calc(Radio.RXdataBuffer, 7, CRCInitializer);

    if (inCRC != calculatedCRC)
    {
        DBGV("CRC error: ");
        for (int i = 0; i < 8; i++)
        {
            DBGV("%x,", Radio.RXdataBuffer[i]);
        }
        DBGVCR;
        #if defined(DEBUG_RX_SCOREBOARD)
            lastPacketCrcError = true;
        #endif
        return;
    }
    PFDloop.extEvent(beginProcessing + PACKET_TO_TOCK_SLACK);

    bool telemetryConfirmValue;
    bool currentMspConfirmValue;
    bool doStartTimer = false;
    unsigned long now = millis();

    LastValidPacket = now;

    //getRFlinkInfo(); // now done in Tick timercallback

    switch (type)
    {
    case RC_DATA_PACKET: //Standard RC Data Packet
        if (connectionState != disconnected)
        {
            telemetryConfirmValue = UnpackChannelData(Radio.RXdataBuffer, &crsf,
                NonceRX, TLMratioEnumToValue(ExpressLRS_currAirRate_Modparams->TLMinterval));
            TelemetrySender.ConfirmCurrentPayload(telemetryConfirmValue);
            crsf.sendRCFrameToFC();
        }
        break;

    case MSP_DATA_PACKET:
        currentMspConfirmValue = MspReceiver.GetCurrentConfirm();
        MspReceiver.ReceiveData(Radio.RXdataBuffer[1], Radio.RXdataBuffer + 2);
        if (currentMspConfirmValue != MspReceiver.GetCurrentConfirm())
        {
            NextTelemetryType = ELRS_TELEMETRY_TYPE_LINK;
        }

        if (Radio.RXdataBuffer[1] == 1 && MspData[0] == MSP_ELRS_BIND)
        {
            OnELRSBindMSP(MspData);
            MspReceiver.ResetState();
        }
        else if (MspReceiver.HasFinishedData())
        {
            if (MspData[7] == MSP_SET_RX_CONFIG && MspData[8] == MSP_ELRS_MODEL_ID)
            {
                UpdateModelMatch(MspData[9]);
            }
            else
            {
                crsf.sendMSPFrameToFC(MspData);
                MspReceiver.Unlock();
            }
        }
        break;

    case TLM_PACKET: //telemetry packet from master

        // not implimented yet
        break;

    case SYNC_PACKET: //sync packet from master
         if (Radio.RXdataBuffer[4] == UID[3] && Radio.RXdataBuffer[5] == UID[4] && Radio.RXdataBuffer[6] == (UID[5] ^ config.GetModelId()))
         {
             LastSyncPacket = now;
#if defined(DEBUG_RX_SCOREBOARD)
             DBGW('s');
#endif
             // Will change the packet air rate in loop() if this changes
             ExpressLRS_nextAirRateIndex = (Radio.RXdataBuffer[3] & 0b11000000) >> 6;
             // Update switch mode encoding immediately
             OtaSetSwitchMode((OtaSwitchMode_e)((Radio.RXdataBuffer[3] & 0b00000110) >> 1));
             // Update TLM ratio
             expresslrs_tlm_ratio_e TLMrateIn = (expresslrs_tlm_ratio_e)((Radio.RXdataBuffer[3] & 0b00111000) >> 3);
             if (ExpressLRS_currAirRate_Modparams->TLMinterval != TLMrateIn)
             {
                 DBGLN("New TLMrate: %d", TLMrateIn);
                 ExpressLRS_currAirRate_Modparams->TLMinterval = TLMrateIn;
                 telemBurstValid = false;
             }

             if (connectionState == disconnected
                || NonceRX != Radio.RXdataBuffer[2]
                || FHSSgetCurrIndex() != Radio.RXdataBuffer[1])
             {
                 //DBGLN("\r\n%ux%ux%u", NonceRX, Radio.RXdataBuffer[2], Radio.RXdataBuffer[1]);
                 FHSSsetCurrIndex(Radio.RXdataBuffer[1]);
                 NonceRX = Radio.RXdataBuffer[2];
                 // only in disconnected state the timer is not running
                 if (connectionState == disconnected) { doStartTimer = true; }
                 TentativeConnection(now);
             }
         }
         break;

    default: // code to be executed if n doesn't match any cases
        break;
    }

    LQCalc.add(); // Received a packet, that's the definition of LQ
    // Extend sync duration since we've received a packet at this rate
    // but do not extend it indefinitely
    RFmodeCycleMultiplier = RFmodeCycleMultiplierSlow;

    doneProcessing = micros();
#if defined(DEBUG_RX_SCOREBOARD)
    if (type != SYNC_PACKET) DBGW('R');
#endif
    if (doStartTimer)
        hwTimer.resume(); // will throw an interrupt immediately - there is no check if the timer was already running in the resume() function
}

void beginWebsever()
{
#if defined(PLATFORM_ESP32) || defined(PLATFORM_ESP8266)
    hwTimer.stop();
    BeginWebUpdate();
    webUpdateMode = true;
#endif
}

void sampleButton(unsigned long now)
{
#ifdef GPIO_PIN_BUTTON
    bool buttonValue = digitalRead(GPIO_PIN_BUTTON);

    if (buttonValue == false && buttonPrevValue == true)
    { //falling edge
        buttonDown = true;
    }

    if (buttonValue == true && buttonPrevValue == false)
    { //rising edge
        buttonDown = false;
    }

    if ((now > buttonLastPressed + WEB_UPDATE_PRESS_INTERVAL) && buttonDown)
    { // button held down for WEB_UPDATE_PRESS_INTERVAL
        if (!webUpdateMode)
        {
            beginWebsever();
        }
    }
    if ((now > buttonLastPressed + BUTTON_RESET_INTERVAL) && buttonDown)
    {
#ifdef PLATFORM_ESP8266
        ESP.restart();
#endif
    }
    buttonPrevValue = buttonValue;
#endif
}

void ICACHE_RAM_ATTR RXdoneISR()
{
    ProcessRFPacket();
}

void ICACHE_RAM_ATTR TXdoneISR()
{
    Radio.RXnb();
#if defined(DEBUG_RX_SCOREBOARD)
    DBGW('T');
#endif
}

static void setupSerial()
{
#ifdef PLATFORM_STM32
#if defined(TARGET_R9SLIMPLUS_RX)
    CRSF_RX_SERIAL.setRx(GPIO_PIN_RCSIGNAL_RX);
    CRSF_RX_SERIAL.begin(CRSF_RX_BAUDRATE);

    CRSF_TX_SERIAL.setTx(GPIO_PIN_RCSIGNAL_TX);
#else /* !TARGET_R9SLIMPLUS_RX */
    CRSF_TX_SERIAL.setTx(GPIO_PIN_RCSIGNAL_TX);
    CRSF_TX_SERIAL.setRx(GPIO_PIN_RCSIGNAL_RX);
#endif /* TARGET_R9SLIMPLUS_RX */
#if defined(TARGET_RX_GHOST_ATTO_V1)
    // USART1 is used for RX (half duplex)
    CRSF_RX_SERIAL.setHalfDuplex();
    CRSF_RX_SERIAL.setTx(GPIO_PIN_RCSIGNAL_RX);
    CRSF_RX_SERIAL.begin(CRSF_RX_BAUDRATE);
    CRSF_RX_SERIAL.enableHalfDuplexRx();

    // USART2 is used for TX (half duplex)
    // Note: these must be set before begin()
    CRSF_TX_SERIAL.setHalfDuplex();
    CRSF_TX_SERIAL.setRx((PinName)NC);
    CRSF_TX_SERIAL.setTx(GPIO_PIN_RCSIGNAL_TX);
#endif /* TARGET_RX_GHOST_ATTO_V1 */
    CRSF_TX_SERIAL.begin(CRSF_RX_BAUDRATE);
#endif /* PLATFORM_STM32 */

#if defined(TARGET_RX_FM30_MINI)
    Serial.setRx(GPIO_PIN_DEBUG_RX);
    Serial.setTx(GPIO_PIN_DEBUG_TX);
    Serial.begin(CRSF_RX_BAUDRATE); // Same baud as CRSF for simplicity
#endif

#if defined(PLATFORM_ESP8266)
    Serial.begin(CRSF_RX_BAUDRATE);
#endif

}

static void setupConfigAndPocCheck()
{
    eeprom.Begin();
    config.SetStorageProvider(&eeprom); // Pass pointer to the Config class for access to storage
    config.Load();

#ifndef MY_UID
    // Increment the power on counter in eeprom
    config.SetPowerOnCounter(config.GetPowerOnCounter() + 1);
    config.Commit();

    // If we haven't reached our binding mode power cycles
    // and we've been powered on for 2s, reset the power on counter
    if (config.GetPowerOnCounter() < 3)
    {
        delay(2000);
        config.SetPowerOnCounter(0);
        config.Commit();
    }
#endif
}

static void setupGpio()
{
#ifdef GPIO_PIN_LED_GREEN
    pinMode(GPIO_PIN_LED_GREEN, OUTPUT);
    digitalWrite(GPIO_PIN_LED_GREEN, LOW ^ GPIO_LED_GREEN_INVERTED);
#endif /* GPIO_PIN_LED_GREEN */
#ifdef GPIO_PIN_LED_RED
    pinMode(GPIO_PIN_LED_RED, OUTPUT);
    digitalWrite(GPIO_PIN_LED_RED, LOW ^ GPIO_LED_RED_INVERTED);
#endif /* GPIO_PIN_LED_RED */
#if defined(GPIO_PIN_LED)
    pinMode(GPIO_PIN_LED, OUTPUT);
    digitalWrite(GPIO_PIN_LED, LOW ^ GPIO_LED_RED_INVERTED);
#endif /* GPIO_PIN_LED */
#ifdef GPIO_PIN_BUTTON
    pinMode(GPIO_PIN_BUTTON, INPUT);
#endif /* GPIO_PIN_BUTTON */
#if defined(GPIO_PIN_ANTENNA_SELECT)
    pinMode(GPIO_PIN_ANTENNA_SELECT, OUTPUT);
    digitalWrite(GPIO_PIN_ANTENNA_SELECT, LOW);
#endif
#if defined(TARGET_RX_FM30_MINI)
    pinMode(GPIO_PIN_UART1TX_INVERT, OUTPUT);
    digitalWrite(GPIO_PIN_UART1TX_INVERT, LOW);
#endif
}

static void setupBindingFromConfig()
{
// Use the user defined binding phase if set,
// otherwise use the bind flag and UID in eeprom for UID
#if !defined(MY_UID)
    // Check the byte that indicates if RX has been bound
    if (config.GetIsBound())
    {
        DBGLN("RX has been bound previously, reading the UID from eeprom...");
        const uint8_t* storedUID = config.GetUID();
        for (uint8_t i = 0; i < UID_LEN; ++i)
        {
            UID[i] = storedUID[i];
        }
        DBGLN("UID = %d, %d, %d, %d, %d, %d", UID[0], UID[1], UID[2], UID[3], UID[4], UID[5]);
        CRCInitializer = (UID[4] << 8) | UID[5];
    }
#endif
}

#if defined(PLATFORM_ESP8266)
static void WebUpdateLoop(unsigned long now)
{
    HandleWebUpdate();
    if (now - LEDLastUpdate > LED_INTERVAL_WEB_UPDATE)
    {
        #ifdef GPIO_PIN_LED
        digitalWrite(GPIO_PIN_LED, LED ^ GPIO_LED_RED_INVERTED);
        #endif
        LED = !LED;
        LEDLastUpdate = now;
    }
}
#endif

static void HandleUARTin()
{
    while (CRSF_RX_SERIAL.available())
    {
        telemetry.RXhandleUARTin(CRSF_RX_SERIAL.read());

        if (telemetry.ShouldCallBootloader())
        {
            reset_into_bootloader();
        }
        if (telemetry.ShouldCallEnterBind())
        {
            EnterBindingMode();
        }
        if (telemetry.ShouldCallUpdateModelMatch())
        {
            UpdateModelMatch(telemetry.GetUpdatedModelMatch());
        }
    }
}

static void setupRadio()
{
    Radio.currFreq = GetInitialFreq();
#if !defined(Regulatory_Domain_ISM_2400)
    //Radio.currSyncWord = UID[3];
#endif
    bool init_success = Radio.Begin();
#ifdef PLATFORM_ESP8266
    if (!init_success)
    {
        DBGLN("Failed to detect RF chipset!!!");
        beginWebsever();
        while (1)
        {
            HandleUARTin();
            WebUpdateLoop(millis());
        }
    }
#else // target does not have wifi
    while (!init_success)
    {
        #ifdef GPIO_PIN_LED
        digitalWrite(GPIO_PIN_LED, LED ^ GPIO_LED_RED_INVERTED);
        LED = !LED;
        #endif
        delay(LED_INTERVAL_ERROR);
        DBGLN("Failed to detect RF chipset!!!");
        HandleUARTin();
    }
#endif

    // Set transmit power to maximum
    POWERMGNT P;
    P.init();
    P.setPower(MaxPower);

    Radio.RXdoneCallback = &RXdoneISR;
    Radio.TXdoneCallback = &TXdoneISR;

    // Initialize vars: caused crash on ESP in SetRFLinkRate() where currAirRate is compared
    ExpressLRS_currAirRate_Modparams = get_elrs_airRateConfig(RATE_DEFAULT); 
    ExpressLRS_currAirRate_RFperfParams = get_elrs_RFperfParams(RATE_DEFAULT);

    Radio.LastPacketRSSI = ExpressLRS_currAirRate_RFperfParams->RXsensitivity;

    SetRFLinkRate(RATE_DEFAULT);
    RFmodeCycleMultiplier = 1;
}

static void wifiOff()
{
#ifdef PLATFORM_ESP8266
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
#endif /* PLATFORM_ESP8266 */
}

static void ws2812Blink()
{
 #if WS2812_LED_IS_USED // do startup blinkies for fun
    WS281Binit();
    uint32_t col = 0x0000FF;
    for (uint8_t j = 0; j < 3; j++)
    {
        for (uint8_t i = 0; i < 5; i++)
        {
            WS281BsetLED(col << j*8);
            delay(15);
            WS281BsetLED(0, 0, 0);
            delay(35);
        }
    }
#endif
}

static void updateTelemetryBurst()
{
    if (telemBurstValid)
        return;
    telemBurstValid = true;

    uint32_t hz = RateEnumToHz(ExpressLRS_currAirRate_Modparams->enum_rate);
    uint32_t ratiodiv = TLMratioEnumToValue(ExpressLRS_currAirRate_Modparams->TLMinterval);
    // telemInterval = 1000 / (hz / ratiodiv);
    // burst = TELEM_MIN_LINK_INTERVAL / telemInterval;
    // This ^^^ rearranged to preserve precision vvv
    telemetryBurstMax = TELEM_MIN_LINK_INTERVAL * hz / ratiodiv / 1000U;

    // Reserve one slot for LINK telemetry
    if (telemetryBurstMax > 1)
        --telemetryBurstMax;
    else
        telemetryBurstMax = 1;
    //DBGLN("TLMburst: %d", telemetryBurstMax);

    // Notify the sender to adjust its expected throughput
    TelemetrySender.UpdateTelemetryRate(hz, ratiodiv, telemetryBurstMax);
}

/* If not connected will rotate through the RF modes looking for sync
 * and blink LED
 */
static void cycleRfMode(unsigned long now)
{
    if (connectionState != disconnected || InBindingMode || webUpdateMode) // also never cycle from a tentative connection state
        return;

    // Actually cycle the RF mode if not LOCK_ON_FIRST_CONNECTION
    if (LockRFmode == false && (now - RFmodeLastCycled) > (cycleInterval * RFmodeCycleMultiplier))
    {
        RFmodeLastCycled = now;
        LastSyncPacket = now;           // reset this variable
        SetRFLinkRate(scanIndex % RATE_MAX); // switch between rates
        LQCalc.reset();
        // Display the current air rate to the user as an indicator something is happening
        DBGLN("%d", ExpressLRS_currAirRate_Modparams->interval);
        scanIndex++;
        getRFlinkInfo();
        crsf.sendLinkStatisticsToFC();
        SendLinkStatstoFCintervalLastSent = now - SEND_LINK_STATS_TO_FC_INTERVAL;
        Radio.RXnb();

        // Switch to FAST_SYNC if not already in it (won't be if was just connected)
        RFmodeCycleMultiplier = 1;
    } // if time to switch RF mode

    // Always blink the LED at a steady rate when not connected, independent of the cycle status
    if (now - LEDLastUpdate > LED_INTERVAL_DISCONNECTED)
    {
        #ifdef GPIO_PIN_LED
            digitalWrite(GPIO_PIN_LED, LED ^ GPIO_LED_RED_INVERTED);
        #elif GPIO_PIN_LED_GREEN
            digitalWrite(GPIO_PIN_LED_GREEN, LED ^ GPIO_LED_GREEN_INVERTED);
        #endif
        LED = !LED;
        LEDLastUpdate = now;
    } // if cycle LED
}

void setup()
{
    setupGpio();
    // serial setup must be done before anything as some libs write
    // to the serial port and they'll block if the buffer fills
    setupSerial();
    // Init EEPROM and load config, checking powerup count
    setupConfigAndPocCheck();

    INFOLN("ExpressLRS Module Booting...");

    wifiOff();
    ws2812Blink();
    setupBindingFromConfig();

    FHSSrandomiseFHSSsequence(uidMacSeedGet());

    setupRadio();

    // RFnoiseFloor = MeasureNoiseFloor(); //TODO move MeasureNoiseFloor to driver libs
    // DBGLN("RF noise floor: %d dBm", RFnoiseFloor);

    hwTimer.callbackTock = &HWtimerCallbackTock;
    hwTimer.callbackTick = &HWtimerCallbackTick;

    MspReceiver.SetDataToReceive(ELRS_MSP_BUFFER, MspData, ELRS_MSP_BYTES_PER_CALL);
    Radio.RXnb();
    crsf.Begin();
    hwTimer.init();
    hwTimer.stop();
}

void loop()
{
    unsigned long now = millis();
    HandleUARTin();
    if (hwTimer.running == false)
    {
        crsf.RXhandleUARTout();
    }

    #if defined(PLATFORM_ESP8266) && defined(AUTO_WIFI_ON_INTERVAL)
    if (!disableWebServer && (connectionState == disconnected) && !webUpdateMode && !InBindingMode && now > (AUTO_WIFI_ON_INTERVAL*1000))
    {
        beginWebsever();
    }

    if (!disableWebServer && (connectionState == disconnected) && !webUpdateMode && InBindingMode && now > 60000)
    {
        beginWebsever();
    }

    if (webUpdateMode)
    {
        WebUpdateLoop(now);
        return;
    }
    #endif

    if ((connectionState != disconnected) && (ExpressLRS_nextAirRateIndex != ExpressLRS_currAirRate_Modparams->index)){ // forced change
        LostConnection();
        LastSyncPacket = now;           // reset this variable to stop rf mode switching and add extra time
        RFmodeLastCycled = now;         // reset this variable to stop rf mode switching and add extra time
        DBGLN("Req air rate change %u->%u", ExpressLRS_currAirRate_Modparams->index, ExpressLRS_nextAirRateIndex);
        crsf.sendLinkStatisticsToFC();
        SendLinkStatstoFCintervalLastSent = now - SEND_LINK_STATS_TO_FC_INTERVAL;
    }

    // TODO: timeout should be calculated to make sure at least three syncs will be sent in that timeframe in case we miss exactly these packets (low LQ)
    if (connectionState == tentative && (now - LastSyncPacket > ExpressLRS_currAirRate_RFperfParams->RFmodeCycleAddtionalTime))
    {
        LostConnection();
        DBGLN("Bad sync, aborting");
        RFmodeLastCycled = now;
        LastSyncPacket = now;
    }

    cycleRfMode(now);

    uint32_t localLastValidPacket = LastValidPacket; // Required to prevent race condition due to LastValidPacket getting updated from ISR
    // check if we lost connection
    if ((connectionState == connected) && ((int32_t)ExpressLRS_currAirRate_RFperfParams->RFmodeCycleInterval < (int32_t)(now - localLastValidPacket))) 
    {
        LostConnection();
    }

    // TEST if this is enough --> if we get here we already received a sync packet with correct crc 
    // and were able to find the correct timing for our phaselock
    // TODO: add 3rd condition: count two sync packets with correct data received in a row to double check our signal lock?
    if ((connectionState == tentative) && RXtimerState == tim_locked)
    {
        GotConnection(now);
    }

    if (now > (SendLinkStatstoFCintervalLastSent + SEND_LINK_STATS_TO_FC_INTERVAL))
    {
        if (connectionState == disconnected)
        {
            getRFlinkInfo(); // Timer not running if disconnected -> manually update here
        }
        //if (connectionState != disconnected)
        //{
            crsf.sendLinkStatisticsToFC();
            SendLinkStatstoFCintervalLastSent = now;
        //}
    }

    if (now > (buttonLastSampled + BUTTON_SAMPLE_INTERVAL))
    {
        sampleButton(now);
        buttonLastSampled = now;
    }

#if WS2812_LED_IS_USED
    if ((connectionState == disconnected) && (now - LEDWS2812LastUpdate > 25))
    {
        uint8_t LEDcolor[3] = {0};
        if (LEDfade == 30 || LEDfade == 0)
        {
            LEDfadeDir = !LEDfadeDir;
        }

        LEDfadeDir ? LEDfade = LEDfade + 2 :  LEDfade = LEDfade - 2;
        LEDcolor[(2 - ExpressLRS_currAirRate_Modparams->index) % 3] = LEDfade;
        WS281BsetLED(LEDcolor);
        LEDWS2812LastUpdate = now;
    }
#endif

    // If the eeprom is indicating that we're not bound
    // and we're not already in binding mode, enter binding
    if (!config.GetIsBound() && !InBindingMode)
    {
        INFOLN("RX has not been bound, enter binding mode...");
        EnterBindingMode();
    }

    // If the power on counter is >=3, enter binding and clear counter
#ifndef MY_UID
    if (config.GetPowerOnCounter() >= 3)
    {
        config.SetPowerOnCounter(0);
        config.Commit();

        INFOLN("Power on counter >=3, enter binding mode...");
        EnterBindingMode();
    }
#endif
    // Update the LED while in binding mode
    if (InBindingMode)
    {
        if (now > LEDLastUpdate) // LEDLastUpdate is actually next update here, flagged for refactor
        {
            if (LEDPulseCounter == 0)
            {
                LED = true;
            }
            else if (LEDPulseCounter == 4)
            {
                LED = false;
            }
            else
            {
                LED = !LED;
            }

            if (LEDPulseCounter < 4)
            {
                LEDLastUpdate = now + LED_INTERVAL_BIND_SHORT;
            }
            else
            {
                LEDLastUpdate = now + LED_INTERVAL_BIND_LONG;
                LEDPulseCounter = 0;
            }


            #ifdef GPIO_PIN_LED
            digitalWrite(GPIO_PIN_LED, LED ^ GPIO_LED_RED_INVERTED);
            #endif

            LEDPulseCounter++;
        }
    }

    uint8_t *nextPayload = 0;
    uint8_t nextPlayloadSize = 0;
    if (!TelemetrySender.IsActive() && telemetry.GetNextPayload(&nextPlayloadSize, &nextPayload))
    {
        TelemetrySender.SetDataToTransmit(nextPlayloadSize, nextPayload, ELRS_TELEMETRY_BYTES_PER_CALL);
    }
    updateTelemetryBurst();
}

struct bootloader {
    uint32_t key;
    uint32_t reset_type;
};

void reset_into_bootloader(void)
{
    CRSF_TX_SERIAL.println((const char *)&target_name[4]);
    CRSF_TX_SERIAL.flush();
#if defined(PLATFORM_STM32)
    delay(100);
    DBGLN("Jumping to Bootloader...");
    delay(100);

    /** Write command for firmware update.
     *
     * Bootloader checks this memory area (if newer enough) and
     * perpare itself for fw update. Otherwise it skips the check
     * and starts ELRS firmware immediately
     */
    extern __IO uint32_t _bootloader_data;
    volatile struct bootloader * blinfo = ((struct bootloader*)&_bootloader_data) + 0;
    blinfo->key = 0x454c5253; // ELRS
    blinfo->reset_type = 0xACDC;

    HAL_NVIC_SystemReset();
#elif defined(PLATFORM_ESP8266)
    delay(100);
    ESP.rebootIntoUartDownloadMode();
#endif
}

void EnterBindingMode()
{
    if ((connectionState == connected) || InBindingMode) {
        // Don't enter binding if:
        // - we're already connected
        // - we're already binding
        DBGLN("Cannot enter binding mode!");
        return;
    }
    if (webUpdateMode) {
#if defined(PLATFORM_ESP32) || defined(PLATFORM_ESP8266)
        wifiOff();
        webUpdateMode = false;
        Radio.RXdoneCallback = &RXdoneISR;
        Radio.TXdoneCallback = &TXdoneISR;
        Radio.Begin();
        crsf.Begin();
        hwTimer.resume();
#endif
    }

    // Set UID to special binding values
    UID[0] = BindingUID[0];
    UID[1] = BindingUID[1];
    UID[2] = BindingUID[2];
    UID[3] = BindingUID[3];
    UID[4] = BindingUID[4];
    UID[5] = BindingUID[5];

    CRCInitializer = 0;
    InBindingMode = true;

    // Start attempting to bind
    // Lock the RF rate and freq while binding
    SetRFLinkRate(RATE_DEFAULT);
    Radio.SetFrequencyReg(GetInitialFreq());
    // If the Radio Params (including InvertIQ) parameter changed, need to restart RX to take effect
    Radio.RXnb();

    DBGLN("Entered binding mode at freq = %d", Radio.currFreq);
}

void ExitBindingMode()
{
    if (!InBindingMode) {
        // Not in binding mode
        DBGLN("Cannot exit binding mode, not in binding mode!");
        return;
    }

    LostConnection();

    // Force RF cycling to start at the beginning immediately
    scanIndex = RATE_MAX;
    RFmodeLastCycled = 0;

    // Do this last as LostConnection() will wait for a tock that never comes
    // if we're in binding mode
    InBindingMode = false;
}

void OnELRSBindMSP(uint8_t* packet)
{
    for (int i = 1; i <=4; i++)
    {
        UID[i + 1] = packet[i];
    }

    CRCInitializer = (UID[4] << 8) | UID[5];

    DBGLN("New UID = %d, %d, %d, %d, %d, %d", UID[0], UID[1], UID[2], UID[3], UID[4], UID[5]);

    // Set new UID in eeprom
    config.SetUID(UID);

    // Set eeprom byte to indicate RX is bound
    config.SetIsBound(true);

    // Write the values to eeprom
    config.Commit();

    FHSSrandomiseFHSSsequence(uidMacSeedGet());

    disableWebServer = true;
    ExitBindingMode();
}

void UpdateModelMatch(uint8_t model)
{
    config.SetModelId(model);
    config.Commit();
    delay(100);
#if defined(PLATFORM_STM32)
    HAL_NVIC_SystemReset();
#elif defined(PLATFORM_ESP8266)
    ESP.restart();
#endif
}
