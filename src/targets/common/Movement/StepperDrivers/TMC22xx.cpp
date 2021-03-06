/*
 * TMC22xx.cpp
 *
 *  Created on: 23 Jan 2016
 *      Author: David
 * Modified on: 1 Jun 2020 to support TMC2209 (based on Duet expansion board code) on the LPC platform
 *		Author: gloomyandy
 * NOTE: The Duet3d TMC22XX driver now supports TMC2209 devices. However it uses an extra task to run the
 * the driver code which requires an extra 400 bytes of RAM. For now we avoid this by continuing to use
 * Spin to drive the device. We may need to review this at some point.
 */
#include "RepRapFirmware.h"

#if SUPPORT_TMC22xx
#ifndef TMC22xx_HAS_ENABLE_PINS
# error TMC22xx_HAS_ENABLE_PINS not defined
#endif

#ifndef TMC22xx_HAS_MUX
# error TMC22xx_HAS_MUX not defined
#endif

#ifndef TMC22xx_VARIABLE_NUM_DRIVERS
# error TMC22xx_VARIABLE_NUM_DRIVERS not defined
#endif

#ifndef TMC22xx_USE_SLAVEADDR
# error TMC22xx_USE_SLAVEADDR not defined
#endif

#include "TMC22xx.h"
#include "RepRap.h"
#include "Movement/Move.h"
#include "Movement/StepTimer.h"
#include "Cache.h"
#if HAS_STALL_DETECT
#include "Endstops/Endstop.h"
#endif

#if TMC_SOFT_UART
#include "TMCSoftUART.h"
#else
#include "sam/drivers/pdc/pdc.h"
#include "sam/drivers/uart/uart.h"
#ifndef TMC22xx_HAS_MUX
# error TMC22xx_HAS_MUX not defined
#endif
#endif

// Important note:
// The TMC2224 does handle a write request immediately followed by a read request.
// The TMC2224 does _not_ handle back-to-back read requests, it needs a short delay between them.

// Motor current calculations
// Full scale current has two ranges (VSENSE 1 or 0) and is given by
// iMax = 0.32/(RSense + 0.02) (for VSENSE 0) iMax = 0.18/(RSense + 0.02) (for VSENSE = 1)
// On typical TMC2209 driver boards RSense is 0.11Ohms on the Duet it is 0.082 Ohms
#if __LPC17xx__ || STM32F4
constexpr float RSense = 0.11;
#else
constexpr float RSense = 0.082;
#endif
// Which gives iMax values in mA of...
constexpr int32_t iMax_VS1 = (int32_t)((0.18/(RSense + 0.02))*1000 + 0.5);
constexpr int32_t iMax_VS0 = (int32_t)((0.32/(RSense + 0.02))*1000 + 0.5);

constexpr float MaximumMotorCurrent = iMax_VS0;
constexpr float MaximumStandstillCurrent = 1400.0;
constexpr float MinimumOpenLoadMotorCurrent = 500;			// minimum current in mA for the open load status to be taken seriously
constexpr uint32_t DefaultMicrosteppingShift = 4;			// x16 microstepping
constexpr bool DefaultInterpolation = true;					// interpolation enabled
constexpr uint32_t DefaultTpwmthrsReg = 2000;				// low values (high changeover speed) give horrible jerk at the changeover from stealthChop to spreadCycle
constexpr uint32_t MaximumWaitTime = 10;					// Wait time for commands we need to complete
constexpr uint16_t DriverNotPresentTimeouts = 10;			// Number of timeouts before we decide to ignore the driver
#if HAS_STALL_DETECT
const int DefaultStallDetectThreshold = 1;
const unsigned int DefaultMinimumStepsPerSecond = 200;		// for stall detection: 1 rev per second assuming 1.8deg/step, as per the TMC5160 datasheet
#endif

#if TMC22xx_VARIABLE_NUM_DRIVERS

static size_t numTmc22xxDrivers;
static inline size_t GetNumTmcDrivers() { return numTmc22xxDrivers; }

#else

static inline constexpr size_t GetNumTmcDrivers() { return MaxSmartDrivers; }

#endif

enum class DriversState : uint8_t
{
	noPower = 0,
	notInitialised,
	initialising,
	ready
};

static DriversState driversState = DriversState::noPower;

// GCONF register (0x00, RW)
constexpr uint8_t REGNUM_GCONF = 0x00;
constexpr uint32_t GCONF_USE_VREF = 1 << 0;					// use external VRef
constexpr uint32_t GCONF_INT_RSENSE = 1 << 1;				// use internal sense resistors
constexpr uint32_t GCONF_SPREAD_CYCLE = 1 << 2;				// use spread cycle mode (else stealthchop mode)
constexpr uint32_t GCONF_REV_DIR = 1 << 3;					// reverse motor direction
constexpr uint32_t GCONF_INDEX_OTPW = 1 << 4;				// INDEX output shows over temperature warning (else it shows first microstep position)
constexpr uint32_t GCONF_INDEX_PULSE = 1 << 5;				// INDEX output shows pulses from internal pulse generator, else as set by GCONF_INDEX_OTPW
constexpr uint32_t GCONF_UART = 1 << 6;						// PDN_UART used for UART interface (else used for power down)
constexpr uint32_t GCONF_MSTEP_REG = 1 << 7;				// microstep resolution set by MSTEP register (else by MS1 and MS2 pins)
constexpr uint32_t GCONF_MULTISTEP_FILT = 1 << 8;			// pulse generation optimised for >750Hz full stepping frequency
constexpr uint32_t GCONF_TEST_MODE = 1 << 9;				// test mode, do not set this bit for normal operation

constexpr uint32_t DefaultGConfReg = GCONF_UART | GCONF_MSTEP_REG | GCONF_MULTISTEP_FILT;

// General configuration and status registers

// GSTAT register (0x01, RW). Write 1 bits to clear the flags.
constexpr uint8_t REGNUM_GSTAT = 0x01;
constexpr uint32_t GSTAT_RESET = 1 << 0;					// driver has been reset since last read
constexpr uint32_t GSTAT_DRV_ERR = 1 << 1;					// driver has been shut down due to over temp or short circuit
constexpr uint32_t GSTAT_UV_CP = 1 << 2;					// undervoltage on charge pump, driver disabled. Not latched so does not need to be cleared.

// IFCOUNT register (0x02, RO)
constexpr uint8_t REGNUM_IFCOUNT = 0x02;
constexpr uint32_t IFCOUNT_MASK = 0x000F;					// interface transmission counter

// SLAVECONF register (0x03, WO)
constexpr uint8_t REGNUM_SLAVECONF = 0x03;
constexpr uint32_t SLAVECONF_SENDDLY_8_BITS = 0 << 8;
constexpr uint32_t SLAVECONF_SENDDLY_24_BITS = 2 << 8;
constexpr uint32_t SLAVECONF_SENDDLY_40_BITS = 4 << 8;
constexpr uint32_t SLAVECONF_SENDDLY_56_BITS = 6 << 8;
constexpr uint32_t SLAVECONF_SENDDLY_72_BITS = 8 << 8;
constexpr uint32_t SLAVECONF_SENDDLY_88_BITS = 10 << 8;
constexpr uint32_t SLAVECONF_SENDDLY_104_BITS = 12 << 8;
constexpr uint32_t SLAVECONF_SENDDLY_120_BITS = 14 << 8;

constexpr uint32_t DefaultSlaveConfReg = SLAVECONF_SENDDLY_8_BITS;	// we don't need any delay between transmission and reception

// OTP_PROG register (0x04, WO)
constexpr uint8_t REGNUM_OTP_PROG = 0x04;
constexpr uint32_t OTP_PROG_BIT_SHIFT = 0;
constexpr uint32_t OTP_PROG_BIT_MASK = 7 << OTP_PROG_BIT_SHIFT;
constexpr uint32_t OTP_PROG_BYTE_SHIFT = 4;
constexpr uint32_t OTP_PROG_BYTE_MASK = 3 << OTP_PROG_BYTE_SHIFT;
constexpr uint32_t OTP_PROG_MAGIC = 0xBD << 8;

// OTP_READ register (0x05, RO)
constexpr uint8_t REGNUM_OTP_READ = 0x05;
constexpr uint32_t OTP_READ_BYTE0_SHIFT = 0;
constexpr uint32_t OTP_READ_BYTE0_MASK = 0xFF << OTP_READ_BYTE0_SHIFT;
constexpr uint32_t OTP_READ_BYTE1_SHIFT = 8;
constexpr uint32_t OTP_READ_BYTE1_MASK = 0xFF << OTP_READ_BYTE1_SHIFT;
constexpr uint32_t OTP_READ_BYTE2_SHIFT = 16;
constexpr uint32_t OTP_READ_BYTE2_MASK = 0xFF << OTP_READ_BYTE2_SHIFT;

// IOIN register (0x06, RO)
constexpr uint8_t REGNUM_IOIN = 0x06;
constexpr uint32_t IOIN_220x_ENN = 1 << 0;
constexpr uint32_t IOIN_222x_PDN_UART = 1 << 1;
constexpr uint32_t IOIN_220x_MS1 = 1 << 2;
constexpr uint32_t IOIN_222x_SPREAD = 2 << 1;
constexpr uint32_t IOIN_220x_MS2 = 1 << 3;
constexpr uint32_t IOIN_222x_DIR = 1 << 3;
constexpr uint32_t IOIN_220x_DIAG = 1 << 4;
constexpr uint32_t IOIN_222x_ENN = 1 << 4;
constexpr uint32_t IOIN_222x_STEP = 1 << 5;
constexpr uint32_t IOIN_220x_PDN_UART = 1 << 6;
constexpr uint32_t IOIN_222x_MS1 = 1 << 6;
constexpr uint32_t IOIN_220x_STEP = 1 << 7;
constexpr uint32_t IOIN_222x_MS2 = 1 << 7;
constexpr uint32_t IOIN_IS_220x = 1 << 8;					// 1 if TMC220x, 0 if TMC222x
constexpr uint32_t IOIN_2209_SPREAD_EN = 1 << 8;
constexpr uint32_t IOIN_220x_DIR = 1 << 9;
constexpr uint32_t IOIN_VERSION_SHIFT = 24;
constexpr uint32_t IOIN_VERSION_MASK = 0xFF << IOIN_VERSION_SHIFT;

constexpr uint32_t IOIN_VERSION_2208_2224 = 0x20;			// version for TMC2208/2224
constexpr uint32_t IOIN_VERSION_2209 = 0x21;				// version for TMC2209

// FACTORY_CONF register (0x07, RW)
constexpr uint8_t REGNUM_FACTORY_CONF = 0x07;
constexpr uint32_t FACTORY_CONF_FCLKTRIM_SHIFT = 0;
constexpr uint32_t FACTORY_CONF_FCLKTRIM_MASK = 0x0F << FACTORY_CONF_FCLKTRIM_SHIFT;
constexpr uint32_t FACTORY_CONF_OTTRIM_SHIFT = 8;
constexpr uint32_t FACTORY_CONF_OTTRIM_MASK = 0x03 << FACTORY_CONF_OTTRIM_SHIFT;
constexpr uint32_t FACTORY_CONF_OTTRIM_143_120 = 0x00 << FACTORY_CONF_OTTRIM_SHIFT;
constexpr uint32_t FACTORY_CONF_OTTRIM_150_120 = 0x01 << FACTORY_CONF_OTTRIM_SHIFT;
constexpr uint32_t FACTORY_CONF_OTTRIM_150_143 = 0x02 << FACTORY_CONF_OTTRIM_SHIFT;
constexpr uint32_t FACTORY_CONF_OTTRIM_157_143 = 0x03 << FACTORY_CONF_OTTRIM_SHIFT;

// Velocity dependent control registers

// IHOLD_IRUN register (WO)
constexpr uint8_t REGNUM_IHOLDIRUN = 0x10;
constexpr uint32_t IHOLDIRUN_IHOLD_SHIFT = 0;				// standstill current
constexpr uint32_t IHOLDIRUN_IHOLD_MASK = 0x1F << IHOLDIRUN_IHOLD_SHIFT;
constexpr uint32_t IHOLDIRUN_IRUN_SHIFT = 8;
constexpr uint32_t IHOLDIRUN_IRUN_MASK = 0x1F << IHOLDIRUN_IRUN_SHIFT;
constexpr uint32_t IHOLDIRUN_IHOLDDELAY_SHIFT = 16;
constexpr uint32_t IHOLDIRUN_IHOLDDELAY_MASK = 0x0F << IHOLDIRUN_IHOLDDELAY_SHIFT;

constexpr uint32_t DefaultIholdIrunReg = (0 << IHOLDIRUN_IHOLD_SHIFT) | (0 << IHOLDIRUN_IRUN_SHIFT) | (2 << IHOLDIRUN_IHOLDDELAY_SHIFT);
															// approx. 0.5 sec motor current reduction to low power

constexpr uint8_t REGNUM_TPOWER_DOWN = 0x11;	// wo, 8 bits, sets delay from standstill detection to motor current reduction
constexpr uint8_t REGNUM_TSTEP = 0x12;			// ro, 20 bits, measured time between two 1/256 microsteps, in clocks
constexpr uint8_t REGNUM_TPWMTHRS = 0x13;		// wo, 20 bits, upper velocity for StealthChop mode
constexpr uint8_t REGNUM_VACTUAL = 0x22;		// wo, 24 bits signed, sets motor velocity for continuous rotation

// Stallguard registers (TMC2209 only)
constexpr uint8_t REGNUM_TCOOLTHRS = 0x14;		// wo, 20-bit lower threshold velocity. CoolStep and the StallGuard DIAG output are enabled above this speed.
constexpr uint8_t REGNUM_SGTHRS = 0x40;			// w0, 8-bit stall detection threshold. Stall is signalled when SG_RESULT <= SGTHRS * 2.
constexpr uint8_t REGNUM_SG_RESULT = 0x41;		// 10-bit StallGard result, read-only. Bits 0 and 9 are always 0.
constexpr uint8_t REGNUM_COOLCONF = 0x42;		// 16-bit CoolStep control

constexpr uint32_t SG_RESULT_MASK = 1023;

// Minimum StallGuard value. Current is increased if SGRESULT < SEMIN * 32.
constexpr unsigned int COOLCONF_SEMIN_SHIFT = 0;
constexpr uint32_t COOLCONF_SEMIN_MASK = 0x000F << COOLCONF_SEMIN_SHIFT;
// Current increment steps per measured SG_RESULT value: 1,2,4,8
constexpr unsigned int COOLCONF_SEUP_SHIFT = 5;
constexpr uint32_t COOLCONF_SEMUP_MASK = 0x0003 << COOLCONF_SEUP_SHIFT;
// Hysteresis value for smart current control. Motor current is reduced if SG_RESULT >= (SEMIN+SEMAX+1)*32.
constexpr unsigned int COOLCONF_SEMAX_SHIFT = 8;
constexpr uint32_t COOLCONF_SEMAX_MASK = 0x000F << COOLCONF_SEMAX_SHIFT;
// Current down step speed. For each {32,8,2,1} SG_RESULT value, decrease by one
constexpr unsigned int COOLCONF_SEDN_SHIFT = 13;
constexpr uint32_t COOLCONF_SEDN_MASK = 0x0003 << COOLCONF_SEDN_SHIFT;
// Minimum current for smart current control, 0 = half of IRUN, 1 = 1/4 of IRUN
constexpr unsigned int COOLCONF_SEIMIN_SHIFT = 15;
constexpr uint32_t COOLCONF_SEIMIN_MASK = 0x0001 << COOLCONF_SEIMIN_SHIFT;

// Sequencer registers (read only)
constexpr uint8_t REGNUM_MSCNT = 0x6A;
constexpr uint8_t REGNUM_MSCURACT = 0x6B;

// Chopper control registers

// CHOPCONF register
constexpr uint8_t REGNUM_CHOPCONF = 0x6C;
constexpr uint32_t CHOPCONF_TOFF_SHIFT = 0;					// off time setting, 0 = disable driver
constexpr uint32_t CHOPCONF_TOFF_MASK = 0x0F << CHOPCONF_TOFF_SHIFT;
constexpr uint32_t CHOPCONF_HSTRT_SHIFT = 4;				// hysteresis start
constexpr uint32_t CHOPCONF_HSTRT_MASK = 0x07 << CHOPCONF_HSTRT_SHIFT;
constexpr uint32_t CHOPCONF_HEND_SHIFT = 7;					// hysteresis end
constexpr uint32_t CHOPCONF_HEND_MASK = 0x0F << CHOPCONF_HEND_SHIFT;
constexpr uint32_t CHOPCONF_TBL_SHIFT = 15;					// blanking time
constexpr uint32_t CHOPCONF_TBL_MASK = 0x03 << CHOPCONF_TBL_SHIFT;
constexpr uint32_t CHOPCONF_VSENSE_HIGH = 1 << 17;			// use high sensitivity current scaling
constexpr uint32_t CHOPCONF_MRES_SHIFT = 24;				// microstep resolution
constexpr uint32_t CHOPCONF_MRES_MASK = 0x0F << CHOPCONF_MRES_SHIFT;
constexpr uint32_t CHOPCONF_INTPOL = 1 << 28;				// use interpolation
constexpr uint32_t CHOPCONF_DEDGE = 1 << 29;				// step on both edges
constexpr uint32_t CHOPCONF_DISS2G = 1 << 30;				// disable short to ground protection
constexpr uint32_t CHOPCONF_DISS2VS = 1 << 31;				// disable low side short protection

constexpr uint32_t DefaultChopConfReg = 0x10000053 | CHOPCONF_VSENSE_HIGH;	// this is the reset default + CHOPCONF_VSENSE_HIGH - try it until we find something better

// DRV_STATUS register. See the .h file for the bit definitions.
constexpr uint8_t REGNUM_DRV_STATUS = 0x6F;

// PWMCONF register
constexpr uint8_t REGNUM_PWMCONF = 0x70;

constexpr uint32_t DefaultPwmConfReg = 0xC10D0024;			// this is the reset default - try it until we find something better

constexpr uint8_t REGNUM_PWM_SCALE = 0x71;
constexpr uint8_t REGNUM_PWM_AUTO = 0x72;

// Send/receive data and CRC stuff

// Data format to write a driver register:
// Byte 0 sync byte, 0x05 (4 LSBs are don't cares but included in CRC)
// Byte 1 slave address, 0x00
// Byte 2 register register address to write | 0x80
// Bytes 3-6 32-bit data, MSB first
// Byte 7 8-bit CRC of bytes 0-6

// Data format to read a driver register:
// Byte 0 sync byte, 0x05 (4 LSBs are don't cares but included in CRC)
// Byte 1 slave address, 0x00
// Byte 2 register address to read (top bit clear)
// Byte 3 8-bit CRC of bytes 0-2

// Reply to a read request:
// Byte 0 sync byte, 0x05
// Byte 1 master address, 0xFF
// Byte 2 register address (top bit clear)
// Bytes 3-6 32-bit data, MSB first
// Byte 7 8-bit CRC

// Add 1 bit to a CRC
static inline constexpr uint8_t CRCAddBit(uint8_t crc, uint8_t currentByte, uint8_t bit) noexcept
{
	return (((crc ^ (currentByte << (7 - bit))) & 0x80) != 0)
			? (crc << 1) ^ 0x07
				: (crc << 1);
}

// Add a byte to a CRC
static inline constexpr uint8_t CRCAddByte(uint8_t crc, uint8_t currentByte) noexcept
{
	crc = CRCAddBit(crc, currentByte, 0);
	crc = CRCAddBit(crc, currentByte, 1);
	crc = CRCAddBit(crc, currentByte, 2);
	crc = CRCAddBit(crc, currentByte, 3);
	crc = CRCAddBit(crc, currentByte, 4);
	crc = CRCAddBit(crc, currentByte, 5);
	crc = CRCAddBit(crc, currentByte, 6);
	crc = CRCAddBit(crc, currentByte, 7);
	return crc;
}

// CRC of the first 2 bytes we send in any request
static constexpr uint8_t InitialSendCRC = CRCAddByte(CRCAddByte(0, 0x05), 0x00);

// CRC of a request to read the IFCOUNT register
static constexpr uint8_t ReadIfcountCRC = CRCAddByte(InitialSendCRC, REGNUM_IFCOUNT);

//----------------------------------------------------------------------------------------------------------------------------------
// Private types and methods

class TmcDriverState
{
public:
	void Init(uint32_t p_driverNumber, Pin p_enablePin
#if HAS_STALL_DETECT
							, Pin p_diagPin
#endif
			 ) noexcept;
	void SetAxisNumber(size_t p_axisNumber) noexcept;
	uint32_t GetAxisNumber() const noexcept { return axisNumber; }
	void WriteAll() noexcept;
	bool SetMicrostepping(uint32_t shift, bool interpolate) noexcept;
	unsigned int GetMicrostepping(bool& interpolation) const noexcept;		// Get microstepping
	bool SetDriverMode(unsigned int mode) noexcept;
	DriverMode GetDriverMode() const noexcept;
	void SetCurrent(float current) noexcept;
	void Enable(bool en) noexcept;
#if HAS_STALL_DETECT
	void SetStallDetectThreshold(int sgThreshold) noexcept;
	void SetStallMinimumStepsPerSecond(unsigned int stepsPerSecond) noexcept;
	void AppendStallConfig(const StringRef& reply) const noexcept;
#endif
	void AppendDriverStatus(const StringRef& reply) noexcept;
	uint8_t GetDriverNumber() const noexcept { return driverNumber; }
	bool UpdatePending() const noexcept;
	bool UsesGlobalEnable() const noexcept { return enablePin == NoPin; }

	bool SetRegister(SmartDriverRegister reg, uint32_t regVal) noexcept;
	uint32_t GetRegister(SmartDriverRegister reg) const noexcept;

	float GetStandstillCurrentPercent() const noexcept;
	void SetStandstillCurrentPercent(float percent) noexcept;

	void TransferDone() noexcept __attribute__ ((hot));				// called by the ISR when the SPI transfer has completed
	void StartTransfer() noexcept __attribute__ ((hot));				// called to start a transfer
	void TransferTimedOut() noexcept { ++numTimeouts; AbortTransfer(); }
	void DmaError() noexcept { ++numDmaErrors; AbortTransfer(); }
	void AbortTransfer() noexcept;

	uint32_t ReadLiveStatus() const noexcept;
	uint32_t ReadAccumulatedStatus(uint32_t bitsToKeep) noexcept;

	DriversState SetupDriver(bool reset) noexcept;
	static TmcDriverState *GetNextDriver(TmcDriverState *current) noexcept;
	// Variables used by the ISR
	static TmcDriverState * volatile currentDriver;			// volatile because the ISR changes it
	static uint32_t transferStartedTime;

	void UartTmcHandler() noexcept;									// core of the ISR for this driver

private:
	bool SetChopConf(uint32_t newVal) noexcept;
	void UpdateRegister(size_t regIndex, uint32_t regVal) noexcept;
	void UpdateChopConfRegister() noexcept;							// calculate the chopper control register and flag it for sending
	void UpdateCurrent() noexcept;
	void UpdateMaxOpenLoadStepInterval() noexcept;
#if HAS_STALL_DETECT
	bool IsTmc2209() const noexcept { return (readRegisters[ReadIoIn] & IOIN_VERSION_MASK) == (IOIN_VERSION_2209 << IOIN_VERSION_SHIFT); }
	void ResetLoadRegisters() noexcept
	{
		minSgLoadRegister = 1023;
		maxSgLoadRegister = 0;
	}
#endif

#if TMC22xx_HAS_MUX
	void SetUartMux() noexcept;
	static void SetupDMASend(uint8_t regnum, uint32_t outVal, uint8_t crc) noexcept __attribute__ ((hot));	// set up the PDC to send a register
	static void SetupDMAReceive(uint8_t regnum, uint8_t crc) noexcept __attribute__ ((hot));					// set up the PDC to receive a register
#else
	void SetupDMASend(uint8_t regnum, uint32_t outVal, uint8_t crc) noexcept __attribute__ ((hot));			// set up the PDC to send a register
	void SetupDMAReceive(uint8_t regnum, uint8_t crc) noexcept __attribute__ ((hot));						// set up the PDC to receive a register
#endif

#if HAS_STALL_DETECT
	static constexpr unsigned int NumWriteRegisters = 9;		// the number of registers that we write to on a TMC2209
	static constexpr unsigned int NumWriteRegistersNon09 = 6;	// the number of registers that we write to on a TMC2208/2224
#else
	static constexpr unsigned int NumWriteRegisters = 6;		// the number of registers that we write to on a TMC2208/2224
#endif
	static const uint8_t WriteRegNumbers[NumWriteRegisters];	// the register numbers that we write to

	// Write register numbers are in priority order, most urgent first, in same order as WriteRegNumbers
	static constexpr unsigned int WriteGConf = 0;				// microstepping
	static constexpr unsigned int WriteSlaveConf = 1;			// read response timing
	static constexpr unsigned int WriteChopConf = 2;			// enable/disable and microstep setting
	static constexpr unsigned int WriteIholdIrun = 3;			// current setting
	static constexpr unsigned int WritePwmConf = 4;				// read register select, sense voltage high/low sensitivity
	static constexpr unsigned int WriteTpwmthrs = 5;			// upper step rate limit for stealthchop
#if HAS_STALL_DETECT
	static constexpr unsigned int WriteTcoolthrs = 6;			// coolstep and stall DIAG output lower speed threshold
	static constexpr unsigned int WriteSgthrs = 7;				// stallguard threshold
	static constexpr unsigned int WriteCoolconf = 8;			// coolstep configuration
#endif

#if HAS_STALL_DETECT
	static constexpr unsigned int NumReadRegisters = 7;			// the number of registers that we read from on a TMC2209
	static constexpr unsigned int NumReadRegistersNon09 = 6;	// the number of registers that we read from on a TMC2208/2224
#else
	static constexpr unsigned int NumReadRegisters = 6;		// the number of registers that we read from on a TMC2208/2224
#endif
	static const uint8_t ReadRegNumbers[NumReadRegisters];	// the register numbers that we read from

	// Read register numbers, in same order as ReadRegNumbers
	static constexpr unsigned int ReadIoIn = 0;				// includes the version which we use to distinguish TMC2209 from 2208/2224
	static constexpr unsigned int ReadGStat = 1;			// global status
	static constexpr unsigned int ReadDrvStat = 2;			// drive status
	static constexpr unsigned int ReadMsCnt = 3;			// microstep counter
	static constexpr unsigned int ReadPwmScale = 4;			// PWM scaling
	static constexpr unsigned int ReadPwmAuto = 5;			// PWM scaling
#if HAS_STALL_DETECT
	static constexpr unsigned int ReadSgResult = 6;			// stallguard result, TMC2209 only
#endif

	volatile uint32_t writeRegisters[NumWriteRegisters];	// the values we want the TMC22xx writable registers to have
	volatile uint32_t readRegisters[NumReadRegisters];		// the last values read from the TMC22xx readable registers
	volatile uint32_t accumulatedReadRegisters[NumReadRegisters];

	uint32_t configuredChopConfReg;							// the configured chopper control register, in the Enabled state, without the microstepping bits
	volatile uint32_t registersToUpdate;					// bitmap of register indices whose values need to be sent to the driver chip
	uint32_t updateMask;									// mask of allowed update registers

	uint32_t axisNumber;									// the axis number of this driver as used to index the DriveMovements in the DDA
	uint32_t microstepShiftFactor;							// how much we need to shift 1 left by to get the current microstepping
	uint32_t motorCurrent;									// the configured motor current
	uint32_t maxOpenLoadStepInterval;						// the maximum step pulse interval for which we consider open load detection to be reliable

#if HAS_STALL_DETECT
	uint32_t minSgLoadRegister;								// the minimum value of the StallGuard bits we read
	uint32_t maxSgLoadRegister;								// the maximum value of the StallGuard bits we read
	DriversBitmap driverBit;								// bitmap of just this driver number
#endif

#if TMC_SOFT_UART

#elif TMC22xx_HAS_MUX
	static Uart * const uart;								// the UART that controls all drivers
#else
	Uart *uart;												// the UART that controls this driver
#endif

	// To write a register, we send one 8-byte packet to write it, then a 4-byte packet to ask for the IFCOUNT register, then we receive an 8-byte packet containing IFCOUNT.
	// This is the message we send - volatile because we care about when it is written
	static volatile uint8_t sendData[12];

	// Buffer for the message we receive when reading data. The first 4 or 12 bytes bytes are our own transmitted data.
	static volatile uint8_t receiveData[20];

	uint16_t readErrors;									// how many read errors we had
	uint16_t writeErrors;									// how many write errors we had
	uint16_t numReads;										// how many successful reads we had
	uint16_t numWrites;										// how many successful writes we had
	uint16_t numTimeouts;									// how many times a transfer timed out
	uint16_t numDmaErrors;
	Pin enablePin;											// the enable pin of this driver, if it has its own
#if HAS_STALL_DETECT
	Pin diagPin;
#endif
	uint8_t driverNumber;									// the number of this driver as addressed by the UART multiplexer
	uint8_t standstillCurrentFraction;						// divide this by 256 to get the motor current standstill fraction
	uint8_t registerToRead;									// the next register we need to read
	uint8_t maxReadCount;									// max register to read
	uint8_t regnumBeingUpdated;								// which register we are sending
	uint8_t lastIfCount;									// the value of the IFCNT register last time we read it
	uint8_t failedOp;
	volatile uint8_t writeRegCRCs[NumWriteRegisters];		// CRCs of the messages needed to update the registers
	static const uint8_t ReadRegCRCs[NumReadRegisters];		// CRCs of the messages needed to read the registers
	bool enabled;											// true if driver is enabled
};

// Static data members of class TmcDriverState

#if TMC22xx_HAS_MUX
Uart * const TmcDriverState::uart = UART_TMC22xx;
#endif

TmcDriverState * volatile TmcDriverState::currentDriver = nullptr;	// volatile because the ISR changes it
uint32_t TmcDriverState::transferStartedTime;

#if TMC_SOFT_UART
// When using a soft UART other interrupts or disabling interrupts for a length of time can easily
// create errors. Because of this it is likely that we will read corrupt data from time to time. 
// We add an extra layer of data validation by always checking the CRC of the entire packet as
// well as the normal checks.
uint8_t static calcCRC(volatile uint8_t *datagram, uint8_t len) {
	uint8_t crc = 0;
	for (uint8_t i = 0; i < len; i++) {
		uint8_t currentByte = datagram[i];
		for (uint8_t j = 0; j < 8; j++) {
			if ((crc >> 7) ^ (currentByte & 0x01)) {
				crc = (crc << 1) ^ 0x07;
			} else {
				crc = (crc << 1);
			}
			crc &= 0xff;
			currentByte = currentByte >> 1;
		}
	}
	return crc;
}


#endif

// To write a register, we send one 8-byte packet to write it, then a 4-byte packet to ask for the IFCOUNT register, then we receive an 8-byte packet containing IFCOUNT.
// This is the message we send - volatile because we care about when it is written
volatile uint8_t TmcDriverState::sendData[12] =
{
	0x05, 0x00,							// sync byte and slave address
	0x00,								// register address and write flag (filled in)
	0x00, 0x00, 0x00, 0x00,				// value to write (if writing), or 1 byte of CRC if read request (filled in)
	0x00,								// CRC of write request (filled in)
	0x05, 0x00,							// sync byte and slave address
	REGNUM_IFCOUNT,						// register we want to read
	ReadIfcountCRC						// CRC
};

// Buffer for the message we receive when reading data. The first 4 or 12 bytes bytes are our own transmitted data.
volatile uint8_t TmcDriverState::receiveData[20];

constexpr uint8_t TmcDriverState::WriteRegNumbers[NumWriteRegisters] =
{
	REGNUM_GCONF,
	REGNUM_SLAVECONF,
	REGNUM_CHOPCONF,
	REGNUM_IHOLDIRUN,
	REGNUM_PWMCONF,
	REGNUM_TPWMTHRS,
#if HAS_STALL_DETECT
	// The rest are on TMC2209 only
	REGNUM_TCOOLTHRS,
	REGNUM_SGTHRS,
	REGNUM_COOLCONF
#endif
};

constexpr uint8_t TmcDriverState::ReadRegNumbers[NumReadRegisters] =
{
	REGNUM_IOIN,						// tells us whether we have a TMC2208/24 or a TMC2209
	REGNUM_GSTAT,
	REGNUM_DRV_STATUS,
	REGNUM_MSCNT,
	REGNUM_PWM_SCALE,
	REGNUM_PWM_AUTO,
#if HAS_STALL_DETECT
	REGNUM_SG_RESULT					// TMC2209 only
#endif
};

constexpr uint8_t TmcDriverState::ReadRegCRCs[NumReadRegisters] =
{
	CRCAddByte(InitialSendCRC, ReadRegNumbers[0]),
	CRCAddByte(InitialSendCRC, ReadRegNumbers[1]),
	CRCAddByte(InitialSendCRC, ReadRegNumbers[2]),
	CRCAddByte(InitialSendCRC, ReadRegNumbers[3]),
	CRCAddByte(InitialSendCRC, ReadRegNumbers[4]),
	CRCAddByte(InitialSendCRC, ReadRegNumbers[5]),
#if HAS_STALL_DETECT
	CRCAddByte(InitialSendCRC, ReadRegNumbers[6])
#endif
};

// State structures for all drivers
static TmcDriverState driverStates[MaxSmartDrivers];

inline bool TmcDriverState::UpdatePending() const noexcept
{
	return (registersToUpdate & updateMask) != 0 || regnumBeingUpdated != 0xff;
}

// Set up the PDC or DMAC to send a register
inline void TmcDriverState::SetupDMASend(uint8_t regNum, uint32_t regVal, uint8_t crc) noexcept
{
#if !TMC_SOFT_UART
	// Faster code, not using the ASF
	Pdc * const pdc = uart_get_pdc_base(uart);
	pdc->PERIPH_PTCR = (PERIPH_PTCR_RXTDIS | PERIPH_PTCR_TXTDIS);	// disable the PDC
#endif

	sendData[2] = regNum | 0x80;
	sendData[3] = (uint8_t)(regVal >> 24);
	sendData[4] = (uint8_t)(regVal >> 16);
	sendData[5] = (uint8_t)(regVal >> 8);
	sendData[6] = (uint8_t)regVal;
	sendData[7] = crc;

#if TMC_SOFT_UART
	TMCSoftUARTStartTransfer(driverNumber, sendData, 12, receiveData + 12, 8);
#else
	Cache::FlushBeforeDMASend(sendData, sizeof(sendData));
	Cache::FlushBeforeDMAReceive(receiveData, sizeof(receiveData));

	pdc->PERIPH_TPR = reinterpret_cast<uint32_t>(sendData);
	pdc->PERIPH_TCR = 12;											// number of bytes to send: 8 bytes send request + 4 bytes read IFCOUNT request

	pdc->PERIPH_RPR = reinterpret_cast<uint32_t>(receiveData);
	pdc->PERIPH_RCR = 20;											// number of bytes to receive: the sent data + 8 bytes of received data

	pdc->PERIPH_PTCR = (PERIPH_PTCR_RXTEN | PERIPH_PTCR_TXTEN);		// enable the PDC to transmit and receive
#endif
}

// Set up the PDC or DMAC to send a register and receive the status
inline void TmcDriverState::SetupDMAReceive(uint8_t regNum, uint8_t crc) noexcept
{
#if !TMC_SOFT_UART
	// Faster code, not using the ASF
	Pdc * const pdc = uart_get_pdc_base(uart);
	pdc->PERIPH_PTCR = (PERIPH_PTCR_RXTDIS | PERIPH_PTCR_TXTDIS);	// disable the PDC
#endif

	sendData[2] = regNum;
	sendData[3] = crc;

#if TMC_SOFT_UART
	TMCSoftUARTStartTransfer(driverNumber, sendData, 4, receiveData + 4, 8);
#else
	Cache::FlushBeforeDMASend(sendData, sizeof(sendData));
	Cache::FlushBeforeDMAReceive(receiveData, sizeof(receiveData));

	pdc->PERIPH_TPR = reinterpret_cast<uint32_t>(sendData);
	pdc->PERIPH_TCR = 4;											// send a 4 byte read data request

	pdc->PERIPH_RPR = reinterpret_cast<uint32_t>(receiveData);
	pdc->PERIPH_RCR = 12;											// receive the 4 bytes we sent + 8 bytes of received data

	pdc->PERIPH_PTCR = (PERIPH_PTCR_RXTEN | PERIPH_PTCR_TXTEN);		// enable the PDC to transmit and receive
#endif
}

// Update the maximum step pulse interval at which we consider open load detection to be reliable
void TmcDriverState::UpdateMaxOpenLoadStepInterval() noexcept
{
	const uint32_t defaultMaxInterval = StepTimer::StepClockRate/MinimumOpenLoadFullStepsPerSec;
	if ((writeRegisters[WriteGConf] & GCONF_SPREAD_CYCLE) != 0)
	{
		maxOpenLoadStepInterval = defaultMaxInterval;
	}
	else
	{
		// In stealthchop mode open load detection in unreliable, so disable it below the speed at which we switch to spreadCycle
		const uint32_t tpwmthrs = writeRegisters[WriteTpwmthrs] & 0x000FFFFF;
		// tpwmthrs is the 20-bit interval between 1/256 microsteps threshold, in clock cycles @ 12MHz.
		// We need to convert it to the interval between full steps, measured in our step clocks, less about 20% to allow some margin.
		// So multiply by the step clock rate divided by 12MHz, also multiply by 256 less 20%.
		constexpr uint32_t conversionFactor = ((256 - 51) * (StepTimer::StepClockRate/1000000))/12;
		const uint32_t fullStepClocks = tpwmthrs * conversionFactor;
		maxOpenLoadStepInterval = min<uint32_t>(fullStepClocks, defaultMaxInterval);
	}
}

// Set a register value and flag it for updating
void TmcDriverState::UpdateRegister(size_t regIndex, uint32_t regVal) noexcept
{
	registersToUpdate &= ~(1u << regIndex);								// make sure it is not updated while we are changing it
	uint8_t crc = InitialSendCRC;
	crc = CRCAddByte(crc, WriteRegNumbers[regIndex] | 0x80);
	crc = CRCAddByte(crc, (uint8_t)(regVal >> 24));
	crc = CRCAddByte(crc, (uint8_t)(regVal >> 16));
	crc = CRCAddByte(crc, (uint8_t)(regVal >> 8));
	crc = CRCAddByte(crc, (uint8_t)regVal);
	const irqflags_t flags = cpu_irq_save();
	writeRegisters[regIndex] = regVal;
	writeRegCRCs[regIndex] = crc;
	registersToUpdate |= (1u << regIndex);								// flag it for sending
	cpu_irq_restore(flags);
	if (regIndex == WriteGConf || regIndex == WriteTpwmthrs)
	{
		UpdateMaxOpenLoadStepInterval();
	}
}

// Calculate the chopper control register and flag it for sending
void TmcDriverState::UpdateChopConfRegister() noexcept
{
	UpdateRegister(WriteChopConf, (enabled) ? configuredChopConfReg : configuredChopConfReg & ~CHOPCONF_TOFF_MASK);
}

// Initialise the state of the driver and its CS pin
void TmcDriverState::Init(uint32_t p_driverNumber, Pin p_enablePin
#if HAS_STALL_DETECT
							, Pin p_diagPin
#endif
) noexcept
pre(!driversPowered)
{
	driverNumber = p_driverNumber;
	axisNumber = p_driverNumber;										// assume straight-through axis mapping initially
	enablePin = p_enablePin;											// this is NoPin for the built-in drivers
	IoPort::SetPinMode(p_enablePin, OUTPUT_HIGH);

#if HAS_STALL_DETECT
	driverBit = DriversBitmap::MakeFromBits(driverNumber);
	diagPin = p_diagPin;
	IoPort::SetPinMode(p_diagPin, INPUT_PULLUP);
#endif

#if !TMC22xx_HAS_MUX && !TMC_SOFT_UART
	uart = TMC22xxUarts[p_driverNumber];
#endif


	enabled = false;
	registersToUpdate = 0;
	motorCurrent = 0;
	standstillCurrentFraction = (256 * 3)/4;							// default to 75%
	UpdateRegister(WriteGConf, DefaultGConfReg);
	UpdateRegister(WriteSlaveConf, DefaultSlaveConfReg);
	configuredChopConfReg = DefaultChopConfReg;
	SetMicrostepping(DefaultMicrosteppingShift, DefaultInterpolation);	// this also updates the chopper control register
	UpdateRegister(WriteIholdIrun, DefaultIholdIrunReg);
	UpdateRegister(WritePwmConf, DefaultPwmConfReg);
	UpdateRegister(WriteTpwmthrs, DefaultTpwmthrsReg);
#if HAS_STALL_DETECT
	SetStallDetectThreshold(DefaultStallDetectThreshold);
	SetStallMinimumStepsPerSecond(DefaultMinimumStepsPerSecond);
	UpdateRegister(WriteCoolconf, 0);									// coolStep disabled
#endif

	for (size_t i = 0; i < NumReadRegisters; ++i)
	{
		accumulatedReadRegisters[i] = readRegisters[i] = 0;				// clear all read registers so that we don't use dud values, in particular we don't know the driver type yet
	}
	regnumBeingUpdated = 0xFF;
	failedOp = 0xFF;
	registerToRead = 0;
	lastIfCount = 0;
	readErrors = writeErrors = numReads = numWrites = numTimeouts = numDmaErrors = 0;
#if HAS_STALL_DETECT
	ResetLoadRegisters();
#endif
}

#if HAS_STALL_DETECT

void TmcDriverState::SetStallDetectThreshold(int sgThreshold) noexcept
{
	// TMC2209 stall threshold is 0 to 255 with 255 being most sensitive.
	// RRF is normally -63 to 64 with -63 being most sensitive
	// We expand the RRF range but adjust it for TMC2209
	const uint32_t sgthrs = 255 - (uint32_t)(constrain<int>(sgThreshold, -128, 127) + 128);
	UpdateRegister(WriteSgthrs, sgthrs);
}

void TmcDriverState::SetStallMinimumStepsPerSecond(unsigned int stepsPerSecond) noexcept
{
	UpdateRegister(WriteTcoolthrs, (12000000 + (128 * stepsPerSecond))/(256 * stepsPerSecond));
}

void TmcDriverState::AppendStallConfig(const StringRef& reply) const noexcept
{
	const int threshold = (int)((255 - writeRegisters[WriteSgthrs]) - 128);
	reply.catf("stall threshold %d, steps/sec %" PRIu32 ", coolstep %" PRIx32,
				threshold, 12000000 / (256 * writeRegisters[WriteTcoolthrs]), writeRegisters[WriteCoolconf] & 0xFFFF);
}

#endif

inline void TmcDriverState::SetAxisNumber(size_t p_axisNumber) noexcept
{
	axisNumber = p_axisNumber;
}

// Write all registers. This is called when the drivers are known to be powered up.
inline void TmcDriverState::WriteAll() noexcept
{
	registersToUpdate = (1 << NumWriteRegisters) - 1;
}

float TmcDriverState::GetStandstillCurrentPercent() const noexcept
{
	return (float)(standstillCurrentFraction * 100)/256;
}

void TmcDriverState::SetStandstillCurrentPercent(float percent) noexcept
{
	standstillCurrentFraction = (uint8_t)constrain<long>(lrintf((percent * 256)/100), 0, 255);
	UpdateCurrent();
}

// Set the microstepping and microstep interpolation. The desired microstepping is (1 << shift).
bool TmcDriverState::SetMicrostepping(uint32_t shift, bool interpolate) noexcept
{
	microstepShiftFactor = shift;
	configuredChopConfReg = (configuredChopConfReg & ~(CHOPCONF_MRES_MASK | CHOPCONF_INTPOL)) | ((8 - shift) << CHOPCONF_MRES_SHIFT);
	if (interpolate)
	{
		configuredChopConfReg |= CHOPCONF_INTPOL;
	}
	UpdateChopConfRegister();
	return true;
}

// Get microstepping or chopper control register
unsigned int TmcDriverState::GetMicrostepping(bool& interpolation) const noexcept
{
	interpolation = (writeRegisters[WriteChopConf] & CHOPCONF_INTPOL) != 0;
	return 1u << microstepShiftFactor;
}

bool TmcDriverState::SetRegister(SmartDriverRegister reg, uint32_t regVal) noexcept
{
	switch(reg)
	{
	case SmartDriverRegister::chopperControl:
		return SetChopConf(regVal);

	case SmartDriverRegister::toff:
		return SetChopConf((configuredChopConfReg & ~CHOPCONF_TOFF_MASK) | ((regVal << CHOPCONF_TOFF_SHIFT) & CHOPCONF_TOFF_MASK));

	case SmartDriverRegister::tblank:
		return SetChopConf((configuredChopConfReg & ~CHOPCONF_TBL_MASK) | ((regVal << CHOPCONF_TBL_SHIFT) & CHOPCONF_TBL_MASK));

	case SmartDriverRegister::hstart:
		return SetChopConf((configuredChopConfReg & ~CHOPCONF_HSTRT_MASK) | ((regVal << CHOPCONF_HSTRT_SHIFT) & CHOPCONF_HSTRT_MASK));

	case SmartDriverRegister::hend:
		return SetChopConf((configuredChopConfReg & ~CHOPCONF_HEND_MASK) | ((regVal << CHOPCONF_HEND_SHIFT) & CHOPCONF_HEND_MASK));

	case SmartDriverRegister::tpwmthrs:
		UpdateRegister(WriteTpwmthrs, regVal & ((1u << 20) - 1));
		return true;

#if HAS_STALL_DETECT
	case SmartDriverRegister::coolStep:
		UpdateRegister(WriteCoolconf, regVal & ((1u << 16) - 1));
		return true;
#endif

	case SmartDriverRegister::hdec:
	default:
		return false;
	}
}

uint32_t TmcDriverState::GetRegister(SmartDriverRegister reg) const noexcept
{
	switch(reg)
	{
	case SmartDriverRegister::chopperControl:
		return configuredChopConfReg & 0x01FFFF;

	case SmartDriverRegister::toff:
		return (configuredChopConfReg & CHOPCONF_TOFF_MASK) >> CHOPCONF_TOFF_SHIFT;

	case SmartDriverRegister::tblank:
		return (configuredChopConfReg & CHOPCONF_TBL_MASK) >> CHOPCONF_TBL_SHIFT;

	case SmartDriverRegister::hstart:
		return (configuredChopConfReg & CHOPCONF_HSTRT_MASK) >> CHOPCONF_HSTRT_SHIFT;

	case SmartDriverRegister::hend:
		return (configuredChopConfReg & CHOPCONF_HEND_MASK) >> CHOPCONF_HEND_SHIFT;

	case SmartDriverRegister::tpwmthrs:
		return writeRegisters[WriteTpwmthrs] & 0x000FFFFF;

	case SmartDriverRegister::mstepPos:
		return readRegisters[ReadMsCnt];

	case SmartDriverRegister::pwmScale:
		return readRegisters[ReadPwmScale];

	case SmartDriverRegister::pwmAuto:
		return readRegisters[ReadPwmAuto];

	case SmartDriverRegister::hdec:
	case SmartDriverRegister::coolStep:
	default:
		return 0;
	}
}

// Set the chopper control register to the settings provided by the user. We allow only the lowest 17 bits to be set.
bool TmcDriverState::SetChopConf(uint32_t newVal) noexcept
{
	const uint32_t offTime = (newVal & CHOPCONF_TOFF_MASK) >> CHOPCONF_TOFF_SHIFT;
	if (offTime == 0 || (offTime == 1 && (newVal & CHOPCONF_TBL_MASK) < (2 << CHOPCONF_TBL_SHIFT)))
	{
		return false;
	}
	const uint32_t hstrt = (newVal & CHOPCONF_HSTRT_MASK) >> CHOPCONF_HSTRT_SHIFT;
	const uint32_t hend = (newVal & CHOPCONF_HEND_MASK) >> CHOPCONF_HEND_SHIFT;
	if (hstrt + hend > 16)
	{
		return false;
	}
	const uint32_t userMask = CHOPCONF_TBL_MASK | CHOPCONF_HSTRT_MASK | CHOPCONF_HEND_MASK | CHOPCONF_TOFF_MASK;	// mask of bits the user is allowed to change
	configuredChopConfReg = (configuredChopConfReg & ~userMask) | (newVal & userMask);
	UpdateChopConfRegister();
	return true;
}

// Set the driver mode
bool TmcDriverState::SetDriverMode(unsigned int mode) noexcept
{
	switch (mode)
	{
	case (unsigned int)DriverMode::spreadCycle:
		UpdateRegister(WriteGConf, writeRegisters[WriteGConf] | GCONF_SPREAD_CYCLE);
		return true;

	case (unsigned int)DriverMode::stealthChop:
		UpdateRegister(WriteGConf, writeRegisters[WriteGConf] & ~GCONF_SPREAD_CYCLE);
		return true;

	default:
		return false;
	}
}

// Get the driver mode
DriverMode TmcDriverState::GetDriverMode() const noexcept
{
	return ((writeRegisters[WriteGConf] & GCONF_SPREAD_CYCLE) != 0) ? DriverMode::spreadCycle : DriverMode::stealthChop;
}

// Set the motor current
void TmcDriverState::SetCurrent(float current) noexcept
{
	motorCurrent = static_cast<uint32_t>(constrain<float>(current, 50.0, MaximumMotorCurrent));
	UpdateCurrent();
}

void TmcDriverState::UpdateCurrent() noexcept
{
	// Set run and hold currents, adjust vsense as needed to keep bits in range.
	uint32_t vsense;
	uint32_t iRunCsBits;
	uint32_t iHoldCsBits;
	const uint32_t iHoldCurrent = min<uint32_t>((motorCurrent * standstillCurrentFraction)/256, (uint32_t)MaximumStandstillCurrent);	// calculate standstill current

	if (motorCurrent <= iMax_VS1)
	{
		// we can use the high sensitivity setting
	 	iRunCsBits = (32 * motorCurrent - iMax_VS1/2)/iMax_VS1;
		iHoldCsBits = (32 * iHoldCurrent - iMax_VS1/2)/iMax_VS1;
		vsense = CHOPCONF_VSENSE_HIGH;
	}
	else
	{
		// use the standard vsense setting
	 	iRunCsBits = (32 * motorCurrent - iMax_VS0/2)/iMax_VS0;
		iHoldCsBits = (32 * iHoldCurrent - iMax_VS0/2)/iMax_VS0;
		vsense = 0;
	}
	//debugPrintf("TMC current iMax %d %d, set I %d IH %d csBits 0x%x 0x%x vsense 0x%x\n", (int)iMax_VS0, (int)iMax_VS1, (int)motorCurrent, (int)iHoldCurrent, (unsigned)iRunCsBits, (unsigned)iHoldCsBits, (unsigned)vsense);
	UpdateRegister(WriteIholdIrun,
					(writeRegisters[WriteIholdIrun] & ~(IHOLDIRUN_IRUN_MASK | IHOLDIRUN_IHOLD_MASK)) | (iRunCsBits << IHOLDIRUN_IRUN_SHIFT) | (iHoldCsBits << IHOLDIRUN_IHOLD_SHIFT));
	configuredChopConfReg = (configuredChopConfReg & ~CHOPCONF_VSENSE_HIGH) | vsense;
	UpdateChopConfRegister();
}

// Enable or disable the driver
void TmcDriverState::Enable(bool en) noexcept
{
	if (enabled != en)
	{
		enabled = en;
		if (enablePin != NoPin)
		{
			digitalWrite(enablePin, !en);			// we assume that smart drivers always have active low enables
		}
		UpdateChopConfRegister();
	}
}

// Read the status
uint32_t TmcDriverState::ReadLiveStatus() const noexcept
{
	uint32_t ret = readRegisters[ReadDrvStat] & (TMC_RR_OT | TMC_RR_OTPW | TMC_RR_S2G | TMC_RR_OLA | TMC_RR_OLB | TMC_RR_STST | TMC_RR_TEMPBITS);
	if (!enabled)
	{
		ret &= ~(TMC_RR_OLA | TMC_RR_OLB);
	}
#if HAS_STALL_DETECT
	if (IoPort::ReadPin(diagPin))
	{
		ret |= TMC_RR_SG;
	}
#endif
	return ret;
}

// Read the status
uint32_t TmcDriverState::ReadAccumulatedStatus(uint32_t bitsToKeep) noexcept
{
	const uint32_t mask = (enabled) ? 0xFFFFFFFF : ~(TMC_RR_OLA | TMC_RR_OLB);
	bitsToKeep &= mask;
	const irqflags_t flags = cpu_irq_save();
	uint32_t status = accumulatedReadRegisters[ReadDrvStat];
	accumulatedReadRegisters[ReadDrvStat] = (status & bitsToKeep) | readRegisters[ReadDrvStat];		// so that the next call to ReadAccumulatedStatus isn't missing some bits
	cpu_irq_restore(flags);
	status &= (TMC_RR_OT | TMC_RR_OTPW | TMC_RR_S2G | TMC_RR_OLA | TMC_RR_OLB | TMC_RR_STST | TMC_RR_TEMPBITS) & mask;
#if HAS_STALL_DETECT
	if (IoPort::ReadPin(diagPin))
	{
		status |= TMC_RR_SG;
	}
#endif
	return status;
}

// Append the driver status to a string, and reset the min/max load values
void TmcDriverState::AppendDriverStatus(const StringRef& reply) noexcept
{
	if (maxReadCount == 0)
	{
		reply.cat("no-driver-detected");
		return;
	}
	const uint32_t lastReadStatus = readRegisters[ReadDrvStat];
	if (lastReadStatus & TMC_RR_OT)
	{
		reply.cat("temperature-shutdown! ");
	}
	else if (lastReadStatus & TMC_RR_OTPW)
	{
		reply.cat("temperature-warning, ");
	}
	if (lastReadStatus & TMC_RR_S2G)
	{
		reply.cat("short-to-ground, ");
	}
	if (lastReadStatus & TMC_RR_OLA)
	{
		reply.cat("open-load-A, ");
	}
	if (lastReadStatus & TMC_RR_OLB)
	{
		reply.cat("open-load-B, ");
	}
	if (lastReadStatus & TMC_RR_STST)
	{
		reply.cat("standstill, ");
	}
	else if ((lastReadStatus & (TMC_RR_OT | TMC_RR_OTPW | TMC_RR_S2G | TMC_RR_OLA | TMC_RR_OLB)) == 0)
	{
		reply.cat("ok, ");
	}

#if HAS_STALL_DETECT
	if (minSgLoadRegister <= maxSgLoadRegister)
	{
		reply.catf("SG min/max %" PRIu32 "/%" PRIu32, minSgLoadRegister, maxSgLoadRegister);
	}
	else
	{
		reply.cat("SG min/max N/A");
	}
	ResetLoadRegisters();
#endif

	reply.catf(", error r/w %u/%u, ifcnt %u, cnt r/w %u/%u, timeout %u, failedOp 0x%02x",
					readErrors, writeErrors, lastIfCount, numReads, numWrites, numTimeouts, failedOp);
	readErrors = writeErrors = numReads = numWrites = numTimeouts = numDmaErrors = 0;
	failedOp = 0xFF;
}

// This is called by the ISR when the SPI transfer has completed
inline void TmcDriverState::TransferDone() noexcept
{
	if (sendData[2] & 0x80)								// if we were writing a register
	{
		const uint8_t currentIfCount = receiveData[18];
#if TMC_SOFT_UART
		if (regnumBeingUpdated < NumWriteRegisters && currentIfCount == (uint8_t)(lastIfCount + 1) && (sendData[2] & 0x7F) == WriteRegNumbers[regnumBeingUpdated]
				&& calcCRC(receiveData+12, 7) == receiveData[19])

#else
		if (regnumBeingUpdated < NumWriteRegisters && currentIfCount == (uint8_t)(lastIfCount + 1) && (sendData[2] & 0x7F) == WriteRegNumbers[regnumBeingUpdated])
#endif
		{
			++numWrites;
		}
		else
		{
			// mark this to retry
			registersToUpdate |= (1u << regnumBeingUpdated);
			++writeErrors;
		}
		lastIfCount = currentIfCount;
		regnumBeingUpdated = 0xFF;
	}
	else if (driversState != DriversState::noPower)		// we don't check the CRC, so only accept the result if power is still good
	{
#if TMC_SOFT_UART
		if (sendData[2] == ReadRegNumbers[registerToRead] && ReadRegNumbers[registerToRead] == receiveData[6] && receiveData[4] == 0x05 && receiveData[5] == 0xFF && calcCRC(receiveData+4, 7) == receiveData[11])
#else
		if (sendData[2] == ReadRegNumbers[registerToRead] && ReadRegNumbers[registerToRead] == receiveData[6] && receiveData[4] == 0x05 && receiveData[5] == 0xFF)
#endif
		{
			// We asked to read the scheduled read register, and the sync byte, slave address and register number in the received message match
			//TODO here we could check the CRC of the received message, but for now we assume that we won't get any corruption in the 32-bit received data
			uint32_t regVal = ((uint32_t)receiveData[7] << 24) | ((uint32_t)receiveData[8] << 16) | ((uint32_t)receiveData[9] << 8) | receiveData[10];

			if (registerToRead == ReadDrvStat)
			{
				uint32_t interval;
				if (   (regVal & TMC_RR_STST) != 0
					|| (interval = reprap.GetMove().GetStepInterval(axisNumber, microstepShiftFactor)) == 0		// get the full step interval
					|| interval > maxOpenLoadStepInterval
					|| motorCurrent < MinimumOpenLoadMotorCurrent
				   )
				{
					regVal &= ~(TMC_RR_OLA | TMC_RR_OLB);				// open load bits are unreliable at standstill and low speeds
				}
			}
#if HAS_STALL_DETECT
			else if (registerToRead == ReadSgResult)
			{
				const uint32_t sgResult = regVal & SG_RESULT_MASK;
				if (sgResult < minSgLoadRegister)
				{
					minSgLoadRegister = sgResult;
				}
				if (sgResult > maxSgLoadRegister)
				{
					maxSgLoadRegister = sgResult;
				}
			}
#endif
			readRegisters[registerToRead] = regVal;
			accumulatedReadRegisters[registerToRead] |= regVal;

			++registerToRead;
			if (registerToRead >= maxReadCount)			{
				registerToRead = 0;
			}
			++numReads;
		}
		else
		{
			++readErrors;
		}
	}
}

// This is called to abandon the current transfer, if any
void TmcDriverState::AbortTransfer() noexcept
{
#if TMC_SOFT_UART
	TMCSoftUARTAbort();
#else
	uart->UART_IDR = UART_IDR_ENDRX;				// disable end-of-receive interrupt
	uart_get_pdc_base(uart)->PERIPH_PTCR = (PERIPH_PTCR_RXTDIS | PERIPH_PTCR_TXTDIS);	// disable the PDC
	uart->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX | UART_CR_RXDIS | UART_CR_TXDIS | UART_CR_RSTSTA;
#endif
	if (regnumBeingUpdated != 0xff)
	{
		// mark this to retry
		registersToUpdate |= (1u << regnumBeingUpdated);
		regnumBeingUpdated = 0xff;
	}
	failedOp = sendData[2];
}

#if TMC22xx_HAS_MUX

// Set up the UART multiplexer to address the selected driver
inline void TmcDriverState::SetUartMux() noexcept
{
	if ((driverNumber & 0x01) != 0)
	{
		fastDigitalWriteHigh(TMC22xxMuxPins[0]);
	}
	else
	{
		fastDigitalWriteLow(TMC22xxMuxPins[0]);
	}
	if ((driverNumber & 0x02) != 0)
	{
		fastDigitalWriteHigh(TMC22xxMuxPins[1]);
	}
	else
	{
		fastDigitalWriteLow(TMC22xxMuxPins[1]);
	}
	if ((driverNumber & 0x04) != 0)
	{
		fastDigitalWriteHigh(TMC22xxMuxPins[2]);
	}
	else
	{
		fastDigitalWriteLow(TMC22xxMuxPins[2]);
	}
}

#endif

// This is called from the ISR or elsewhere to start a new SPI transfer. Inlined for ISR speed.
inline void TmcDriverState::StartTransfer() noexcept
{
	currentDriver = this;

#if TMC22xx_HAS_MUX
	SetUartMux();
#endif

	// Find which register to send. The common case is when no registers need to be updated.
	if ((registersToUpdate & updateMask) != 0)
	{
		// Write a register
		const size_t regNum = LowestSetBit(registersToUpdate & updateMask);

		// Kick off a transfer for the register to write
#if TMC_SOFT_UART
		regnumBeingUpdated = regNum;
		registersToUpdate &= ~(1 << regNum);
		SetupDMASend(WriteRegNumbers[regNum], writeRegisters[regNum], writeRegCRCs[regNum]);	// set up the PDC
		transferStartedTime = millis();
#else
		const irqflags_t flags = cpu_irq_save();		// avoid race condition
		regnumBeingUpdated = regNum;
		uart->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX;	// reset transmitter and receiver
		SetupDMASend(WriteRegNumbers[regNum], writeRegisters[regNum], writeRegCRCs[regNum]);	// set up the PDC
		uart->UART_IER = UART_IER_ENDRX;				// enable end-of-transfer interrupt
		uart->UART_CR = UART_CR_RXEN | UART_CR_TXEN;	// enable transmitter and receiver
		transferStartedTime = millis();
		cpu_irq_restore(flags);
#endif
	}
	else
	{
		// Read a register
		regnumBeingUpdated = 0xFF;
#if TMC_SOFT_UART
		SetupDMAReceive(ReadRegNumbers[registerToRead], ReadRegCRCs[registerToRead]);	// set up the PDC
		transferStartedTime = millis();
#else
		const irqflags_t flags = cpu_irq_save();		// avoid race condition
		uart->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX;	// reset transmitter and receiver
		SetupDMAReceive(ReadRegNumbers[registerToRead], ReadRegCRCs[registerToRead]);	// set up the PDC
		uart->UART_IER = UART_IER_ENDRX;				// enable end-of-receive interrupt
		uart->UART_CR = UART_CR_RXEN | UART_CR_TXEN;	// enable transmitter and receiver
		transferStartedTime = millis();
		cpu_irq_restore(flags);
#endif
		transferStartedTime = millis();
	}
}

TmcDriverState *TmcDriverState::GetNextDriver(TmcDriverState *current) noexcept
{
	if (GetNumTmcDrivers() == 0)
		return nullptr;
	if (current == nullptr)
		current = driverStates + GetNumTmcDrivers() - 1;
	TmcDriverState *start = current;
	do {
		current++;
		if (current >= driverStates + GetNumTmcDrivers())
			current = driverStates;
		if (current->maxReadCount != 0)
			return current;
	} while(current != start);
	return nullptr;
}

// ISR(s) for the UART(s)

inline void TmcDriverState::UartTmcHandler() noexcept
{
#if !TMC_SOFT_UART
	uart->UART_IDR = UART_IDR_ENDRX;					// disable the PDC interrupt
#endif
	TransferDone();										// tidy up after the transfer we just completed
	TmcDriverState *driver = GetNextDriver(this);
	if (driver && driversState != DriversState::noPower)
	{
		// Power is still good, so send/receive again
		driver->StartTransfer();
	}
	else
	{
		currentDriver = nullptr;						// signal that we are not waiting for an interrupt
	}
}

DriversState TmcDriverState::SetupDriver(bool reset) noexcept
{
	// Step the driver through the setup process and report current state
	//debugPrintf("Setup driver %d cnt %d/%d", GetDriverNumber(), numReads, numWrites);
	if (reset)
	{
		// set initial state send updates and read registers
		maxReadCount = NumReadRegistersNon09;
		updateMask = (1 << NumWriteRegistersNon09) - 1;
		readErrors = writeErrors = numReads = numWrites = numTimeouts = numDmaErrors = 0;
		WriteAll();
		//debugPrintf(" reset\n");
		return DriversState::noPower;
	}
	// have we disabled this device because of timeouts?
	if (maxReadCount == 0)
	{
		//debugPrintf(" disabled\n");
		return DriversState::notInitialised;
	}
	// check for device not present
	if (numTimeouts > DriverNotPresentTimeouts)
	{
		//debugPrintf(" disabling driver\n");
		maxReadCount = 0;
		return DriversState::notInitialised;
	}

	if (UpdatePending())
	{
		//debugPrintf(" write pending %x\n", registersToUpdate);
		return DriversState::initialising;
	}
	if (numReads >= 1)
	{
		// we have read the basic registers so can work out what device we have
		if (IsTmc2209() && maxReadCount != NumReadRegisters)
		{
			//debugPrintf(" request 2209 reg\n");
			// request extra 2209 registers, note this may trigger more writes
			maxReadCount = NumReadRegisters;
			updateMask = (1 << NumWriteRegisters) - 1;
			return DriversState::initialising;
		}
		if (numReads >= maxReadCount)
		{
			registersToUpdate &= updateMask;
			//debugPrintf(" ready\n");
			return DriversState::ready;
		}
	}
	//debugPrintf(" waiting\n");
	return DriversState::initialising;
}

#if TMC22xx_HAS_MUX

// ISR for the single UART
extern "C" void TMC22xx_UART_Handler() __attribute__ ((hot));

void TMC22xx_UART_Handler()
{
	UART_TMC22xx->UART_IDR = UART_IDR_ENDRX;				// disable the interrupt
	TmcDriverState * const driver = TmcDriverState::currentDriver;	// capture volatile variable
	if (driver != nullptr)
	{
		driver->UartTmcHandler();
	}
}

#elif !TMC_SOFT_UART

// ISRs for the individual UARTs
extern "C" void UART_TMC_DRV0_Handler() __attribute__ ((hot));
void UART_TMC_DRV0_Handler()
{
	driverStates[0].UartTmcHandler();
}

extern "C" void UART_TMC_DRV1_Handler() __attribute__ ((hot));
void UART_TMC_DRV1_Handler()
{
	driverStates[1].UartTmcHandler();
}

#endif

//--------------------------- Public interface ---------------------------------
// Initialise the driver interface and the drivers, leaving each drive disabled.
// It is assumed that the drivers are not powered, so driversPowered(true) must be called after calling this before the motors can be moved.
#if TMC22xx_VARIABLE_NUM_DRIVERS
void SmartDrivers::Init(size_t numTmcDrivers) noexcept
#else
void SmartDrivers::Init() noexcept
#endif
{
#if TMC22xx_VARIABLE_NUM_DRIVERS
	numTmc22xxDrivers = min<size_t>(numTmcDrivers, MaxSmartDrivers);
#endif
	// Make sure the ENN pins are high
#if TMC_SOFT_UART
	TMCSoftUARTInit();
#elif TMC22xx_HAS_MUX
	// Set up- the single UART that communicates with all TMC22xx drivers
	ConfigurePin(TMC22xx_UART_PINS);									// the pins are already set up for UART use in the pins table

	// Enable the clock to the UART
	pmc_enable_periph_clk(ID_TMC22xx_UART);

	// Set the UART baud rate, 8 bits, 2 stop bits, no parity
	UART_TMC22xx->UART_IDR = ~0u;
	UART_TMC22xx->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX | UART_CR_RXDIS | UART_CR_TXDIS;
	UART_TMC22xx->UART_MR = UART_MR_CHMODE_NORMAL | UART_MR_PAR_NO;
	UART_TMC22xx->UART_BRGR = VARIANT_MCK/(16 * DriversBaudRate);		// set baud rate
	UART_TMC22xx->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX | UART_CR_RXDIS | UART_CR_TXDIS | UART_CR_RSTSTA;
	NVIC_EnableIRQ(TMC22xx_UART_IRQn);

	// Set up the multiplexer control pins as outputs
	pinMode(TMC22xxMuxPins[0], OUTPUT_LOW);
	pinMode(TMC22xxMuxPins[1], OUTPUT_LOW);
	pinMode(TMC22xxMuxPins[2], OUTPUT_LOW);
#endif

	driversState = DriversState::noPower;
	for (size_t drive = 0; drive < GetNumTmcDrivers(); ++drive)
	{
#if !TMC22xx_HAS_MUX && !TMC_SOFT_UART
		// Initialise the UART that controls this driver
		// The pins are already set up for UART use in the pins table
		ConfigurePin(TMC22xxUartPins[drive]);

		// Enable the clock to the UART
		pmc_enable_periph_clk(TMC22xxUartIds[drive]);

		// Set the UART baud rate, 8 bits, 2 stop bits, no parity
		Uart * const uart = TMC22xxUarts[drive];
		uart->UART_IDR = ~0u;
		uart->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX | UART_CR_RXDIS | UART_CR_TXDIS;
		uart->UART_MR = UART_MR_CHMODE_NORMAL | UART_MR_PAR_NO;
		uart->UART_BRGR = VARIANT_MCK/(16 * DriversBaudRate);		// set baud rate
		uart->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX | UART_CR_RXDIS | UART_CR_TXDIS | UART_CR_RSTSTA;
		NVIC_EnableIRQ(TMC22xxUartIRQns[drive]);
#endif
		driverStates[drive].Init(drive
#if TMC22xx_HAS_ENABLE_PINS
								, ENABLE_PINS[drive]
#endif
#if HAS_STALL_DETECT
								, DriverDiagPins[drive]
#endif
								);
	}
}

// Shut down the drivers and stop any related interrupts. Don't call Spin() again after calling this as it may re-enable them.
void SmartDrivers::Exit() noexcept
{
	if (GlobalTmc22xxEnablePin != NoPin)
		pinMode(GlobalTmc22xxEnablePin, OUTPUT_HIGH);
#if TMC_SOFT_UART
	TMCSoftUARTShutdown();
#elif TMC22xx_HAS_MUX
	NVIC_DisableIRQ(TMC22xx_UART_IRQn);
#else
	for (size_t drive = 0; drive < GetNumTmcDrivers(); ++drive)
	{
		NVIC_DisableIRQ(TMC22xxUartIRQns[drive]);
	}
#endif
	driversState = DriversState::noPower;
}

void SmartDrivers::SetAxisNumber(size_t drive, uint32_t axisNumber) noexcept
{
	if (drive < GetNumTmcDrivers())
	{
		driverStates[drive].SetAxisNumber(axisNumber);
	}
}

uint32_t SmartDrivers::GetAxisNumber(size_t drive) noexcept
{
	return (drive < GetNumTmcDrivers()) ? driverStates[drive].GetAxisNumber() : 0;
}

void SmartDrivers::SetCurrent(size_t drive, float current) noexcept
{
	if (drive < GetNumTmcDrivers())
	{
		driverStates[drive].SetCurrent(current);
	}
}

void SmartDrivers::EnableDrive(size_t drive, bool en) noexcept
{
	if (drive < GetNumTmcDrivers())
	{
		driverStates[drive].Enable(en);
	}
}

uint32_t SmartDrivers::GetLiveStatus(size_t drive) noexcept
{
	return (drive < GetNumTmcDrivers()) ? driverStates[drive].ReadLiveStatus() : 0;
}

uint32_t SmartDrivers::GetAccumulatedStatus(size_t drive, uint32_t bitsToKeep) noexcept
{
	return (drive < GetNumTmcDrivers()) ? driverStates[drive].ReadAccumulatedStatus(bitsToKeep) : 0;
}

// Set microstepping or chopper control register
bool SmartDrivers::SetMicrostepping(size_t drive, unsigned int microsteps, bool interpolate) noexcept
{
	if (drive < GetNumTmcDrivers() && microsteps > 0)
	{
		// Set the microstepping. We need to determine how many bits right to shift the desired microstepping to reach 1.
		unsigned int shift = 0;
		unsigned int uSteps = (unsigned int)microsteps;
		while ((uSteps & 1) == 0)
		{
			uSteps >>= 1;
			++shift;
		}
		if (uSteps == 1 && shift <= 8)
		{
			driverStates[drive].SetMicrostepping(shift, interpolate);
			return true;
		}
	}
	return false;
}

// Get microstepping or chopper control register
unsigned int SmartDrivers::GetMicrostepping(size_t drive, bool& interpolation) noexcept
{
	return (drive < GetNumTmcDrivers()) ? driverStates[drive].GetMicrostepping(interpolation) : 1;
}

bool SmartDrivers::SetDriverMode(size_t driver, unsigned int mode) noexcept
{
	return driver < GetNumTmcDrivers() && driverStates[driver].SetDriverMode(mode);
}

DriverMode SmartDrivers::GetDriverMode(size_t driver) noexcept
{
	return (driver < GetNumTmcDrivers()) ? driverStates[driver].GetDriverMode() : DriverMode::unknown;
}


// Flag that the the drivers have been powered up or down and handle any timeouts
// Before the first call to this function with 'powered' true, you must call Init()
void SmartDrivers::Spin(bool powered) noexcept
{
#if TMC_SOFT_UART
	if (TMCSoftUARTCheckComplete())
	{
		TmcDriverState *driver = TmcDriverState::currentDriver;	// capture volatile variable
		if (driver != nullptr)
		{
			driver->UartTmcHandler();
		}

	}
#endif
	if (driversState == DriversState::noPower)
	{
		if (powered)
		{
			// Power to the drivers has been provided or restored, so we need to enable and re-initialise them
			for (size_t drive = 0; drive < GetNumTmcDrivers(); ++drive)
			{
				driverStates[drive].SetupDriver(true);
			}
			driversState = DriversState::initialising;
		}
	}
	else if (powered)
	{
		// If no transfer is in progress, kick one off.
		// If a transfer has timed out, abort it.
		if (TmcDriverState::currentDriver == nullptr)
		{
			// No transfer in progress, so start one
			TmcDriverState *driver = TmcDriverState::GetNextDriver(nullptr);
			if (driver != nullptr)
			{
				// Kick off the first transfer
				driver->StartTransfer();
			}
		}
		else if (millis() - TmcDriverState::transferStartedTime > TransferTimeout)
		{
			// A UART transfer was started but has timed out
			TmcDriverState::currentDriver->TransferTimedOut();
			TmcDriverState *driver = TmcDriverState::GetNextDriver(TmcDriverState::currentDriver);
			TmcDriverState::currentDriver = nullptr;

			if (driver != nullptr)
			{
				// Kick off the first transfer
				driver->StartTransfer();
			}
		}

		if (driversState == DriversState::initialising)
		{
			// If all drivers that share the global enable have been initialised, set the global enable
			bool allInitialised = true;
			for (size_t i = 0; i < GetNumTmcDrivers(); ++i)
			{
				if (driverStates[i].SetupDriver(false) == DriversState::initialising)
				{
					allInitialised = false;
				}
			}

			if (allInitialised)
			{
				driversState = DriversState::ready;
				// report drive status
				for (size_t i = 0; i < GetNumTmcDrivers(); ++i)
				{
					if (driverStates[i].SetupDriver(false) == DriversState::ready)
						reprap.GetPlatform().MessageF(UsbMessage, "Driver %d ready\n", i);
					else
						reprap.GetPlatform().MessageF(UsbMessage, "Warning driver %d not detected check configuration\n", i);
				}
			}
		}
	}
	else
	{
		// We had power but we lost it
		if (GlobalTmc22xxEnablePin != NoPin)
			digitalWrite(GlobalTmc22xxEnablePin, HIGH);			// disable the drivers
		if (TmcDriverState::currentDriver != nullptr)
		{
			TmcDriverState::currentDriver->AbortTransfer();
			TmcDriverState::currentDriver = nullptr;
		}
		driversState = DriversState::noPower;
	}
}

bool SmartDrivers::IsReady() noexcept
{
	return driversState == DriversState::ready;
}

// This is called from the tick ISR, possibly while Spin (with powered either true or false) is being executed
void SmartDrivers::TurnDriversOff() noexcept
{
	// When using TMC2660 drivers, this is called when an over-voltage event occurs, so that we can try to protect the drivers by disabling them.
	// We don't use it with TMC22xx drivers.
}

void SmartDrivers::SetStallThreshold(size_t driver, int sgThreshold) noexcept
{
#if HAS_STALL_DETECT
	if (driver < GetNumTmcDrivers())
	{
		driverStates[driver].SetStallDetectThreshold(sgThreshold);
	}
#endif
}

void SmartDrivers::SetStallFilter(size_t driver, bool sgFilter) noexcept
{
	// Not a supported on TMC2209
}

void SmartDrivers::SetStallMinimumStepsPerSecond(size_t driver, unsigned int stepsPerSecond) noexcept
{
#if HAS_STALL_DETECT
	if (driver < GetNumTmcDrivers())
	{
		driverStates[driver].SetStallMinimumStepsPerSecond(stepsPerSecond);
	}
#endif
}

void SmartDrivers::AppendStallConfig(size_t driver, const StringRef& reply) noexcept
{
#if HAS_STALL_DETECT
	if (driver < GetNumTmcDrivers())
	{
		driverStates[driver].AppendStallConfig(reply);
	}
	else
	{
		reply.cat("no such driver");
	}
#endif
}

void SmartDrivers::AppendDriverStatus(size_t drive, const StringRef& reply) noexcept
{
	if (drive < GetNumTmcDrivers())
	{
		driverStates[drive].AppendDriverStatus(reply);
	}
}

float SmartDrivers::GetStandstillCurrentPercent(size_t drive) noexcept
{
	return (drive < GetNumTmcDrivers()) ? driverStates[drive].GetStandstillCurrentPercent() : 0.0;
}

void SmartDrivers::SetStandstillCurrentPercent(size_t drive, float percent) noexcept
{
	if (drive < GetNumTmcDrivers())
	{
		driverStates[drive].SetStandstillCurrentPercent(percent);
	}
}

bool SmartDrivers::SetRegister(size_t driver, SmartDriverRegister reg, uint32_t regVal) noexcept
{
	return (driver < GetNumTmcDrivers()) && driverStates[driver].SetRegister(reg, regVal);
}

uint32_t SmartDrivers::GetRegister(size_t driver, SmartDriverRegister reg) noexcept
{
	return (driver < GetNumTmcDrivers()) ? driverStates[driver].GetRegister(reg) : 0;
}

#if HAS_STALL_DETECT

DriversBitmap SmartDrivers::GetStalledDrivers(DriversBitmap driversOfInterest) noexcept
{
	DriversBitmap rslt;
	driversOfInterest.Iterate([&rslt](unsigned int driverNumber, unsigned int count)
								{
									if (driverNumber < ARRAY_SIZE(DriverDiagPins) && digitalRead(DriverDiagPins[driverNumber]))
									{
										rslt.SetBit(driverNumber);
									}
								}
							 );
	return rslt;
}
#endif

#endif

// End
