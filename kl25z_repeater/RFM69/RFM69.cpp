// RFM69.cpp
//
// Copyright (C) 2014 Phil Crump
//
// Based on RF22 Copyright (C) 2011 Mike McCauley ported to mbed by Karl Zweimueller
// Based on RFM69 LowPowerLabs (https://github.com/LowPowerLab/RFM69/)


#include "mbed.h"
#include "RFM69.h"
#include "RFM69Config.h"


RFM69::RFM69(PinName slaveSelectPin, PinName mosi, PinName miso, PinName sclk, float tempFudge)
    : _slaveSelectPin(slaveSelectPin),  _spi(mosi, miso, sclk)
{
    _mode = RFM69_MODE_RX;
    _temperatureFudge = tempFudge;
}

boolean RFM69::init()
{
    _slaveSelectPin = 1; // Init nSS
    wait_ms(100);
    _spi.format(8,0);
    _spi.frequency(8000000); // 8MHz
    
    // We should check that we can actually talk to the device here
    
    // Set up device
    for (uint8_t i = 0; CONFIG[i][0] != 255; i++)
        spiWrite(CONFIG[i][0], CONFIG[i][1]);
    
    setMode(_mode);

    // Clear TX/RX Buffer
    _bufLen = 0;

    return true;
}

uint8_t RFM69::spiRead(uint8_t reg)
{
    _slaveSelectPin=0;
    
    _spi.write(reg & ~RFM69_SPI_WRITE_MASK); // Send the address with the write mask off
    uint8_t val = _spi.write(0); // The written value is ignored, reg value is read
    
    _slaveSelectPin = 1;
    return val;
}

void RFM69::spiWrite(uint8_t reg, uint8_t val)
{
    _slaveSelectPin = 0;
    
    _spi.write(reg | RFM69_SPI_WRITE_MASK); // Send the address with the write mask on
    _spi.write(val); // New value follows

    _slaveSelectPin = 1;
}

void RFM69::spiBurstRead(uint8_t reg, uint8_t* dest, uint8_t len)
{
    _slaveSelectPin = 0;
    
    _spi.write(reg & ~RFM69_SPI_WRITE_MASK); // Send the start address with the write mask off
    while (len--)
        *dest++ = _spi.write(0);

    _slaveSelectPin = 1;
}

void RFM69::spiBurstWrite(uint8_t reg, const uint8_t* src, uint8_t len)
{
    _slaveSelectPin = 0;
    
    _spi.write(reg | RFM69_SPI_WRITE_MASK); // Send the start address with the write mask on
    while (len--)
        _spi.write(*src++);
        
    _slaveSelectPin = 1;
}

void RFM69::spiFifoWrite(const uint8_t* src, uint8_t len)
{
    _slaveSelectPin = 0;
    
    // Send the start address with the write mask on
    _spi.write(RFM69_REG_00_FIFO | RFM69_SPI_WRITE_MASK);
    // First byte is packet length
    _spi.write(len);
    // Then write the packet
	while (len--)
    	_spi.write(*src++);
    	
    _slaveSelectPin = 1;
}

void RFM69::setMode(uint8_t newMode)
{
    // RFM69_MODE_SLEEP    0.1uA
    // RFM69_MODE_STDBY    1.25mA
    // RFM69_MODE_RX       16mA
    // RFM69_MODE_TX       >33mA
    spiWrite(RFM69_REG_01_OPMODE, (spiRead(RFM69_REG_01_OPMODE) & 0xE3) | newMode);
	_mode = newMode;
}

uint8_t  RFM69::mode()
{
    return _mode;
}

boolean RFM69::checkRx()
{
    // Check IRQ register for payloadready flag (indicates RXed packet waiting in FIFO)
    if(spiRead(RFM69_REG_28_IRQ_FLAGS2) & RF_IRQFLAGS2_PAYLOADREADY) {
        // Get packet length from first byte of FIFO
        _bufLen = spiRead(RFM69_REG_00_FIFO)+1;
        // Read FIFO into our Buffer
        spiBurstRead(RFM69_REG_00_FIFO, _buf, RFM69_FIFO_SIZE);
        // Read RSSI register (should be of the packet? - TEST THIS)
        _lastRssi = -(spiRead(RFM69_REG_24_RSSI_VALUE)/2);
        // Clear the radio FIFO (found in HopeRF demo code)
        clearFifo();
        return true;
    } else {
    	return false;
	}
}

void RFM69::recv(uint8_t* buf, uint8_t* len)
{
    // Copy RX Buffer to byref Buffer
    memcpy(buf, _buf, _bufLen);
    *len = _bufLen;
    // Clear RX Buffer
    _bufLen = 0;
}

void RFM69::send(const uint8_t* data, uint8_t len, uint8_t power)
{
    // power is TX Power in dBmW (valid values are 2dBmW-20dBmW)
    if(power<2 or power >20) {
        // Could be dangerous, so let's check this
        return;
    }
    uint8_t oldMode = _mode;
    // Copy data into TX Buffer
    memcpy(_buf, data, len);
    // Update TX Buffer Size
	_bufLen = len;
	// Start Transmitter
    setMode(RFM69_MODE_TX);
    // Set up PA
    if(power<=17) {
        // Set PA Level
        uint8_t paLevel = power + 14;
	    spiWrite(RFM69_REG_11_PA_LEVEL, RF_PALEVEL_PA0_OFF | RF_PALEVEL_PA1_ON | RF_PALEVEL_PA2_ON | paLevel);        
    } else {
        // Disable Over Current Protection
        spiWrite(RFM69_REG_13_OCP, RF_OCP_OFF);
        // Enable High Power Registers
        spiWrite(RFM69_REG_5A_TEST_PA1, 0x5D);
        spiWrite(RFM69_REG_5C_TEST_PA2, 0x7C);
        // Set PA Level
        uint8_t paLevel = power + 11;
	    spiWrite(RFM69_REG_11_PA_LEVEL, RF_PALEVEL_PA0_OFF | RF_PALEVEL_PA1_ON | RF_PALEVEL_PA2_ON | paLevel);
    }
    // Wait for PA ramp-up
    while(!(spiRead(RFM69_REG_27_IRQ_FLAGS1) & RF_IRQFLAGS1_TXREADY)) { };
    // Throw Buffer into FIFO, packet transmission will start automatically
    spiFifoWrite(_buf, _bufLen);
    // Clear MCU TX Buffer
    _bufLen = 0;
    // Wait for packet to be sent
    while(!(spiRead(RFM69_REG_28_IRQ_FLAGS2) & RF_IRQFLAGS2_PACKETSENT)) { };
    // If we were in high power, switch off High Power Registers
    if(power>17) {
        // Disable High Power Registers
        spiWrite(RFM69_REG_5A_TEST_PA1, 0x55);
        spiWrite(RFM69_REG_5C_TEST_PA2, 0x70);
        // Enable Over Current Protection
        spiWrite(RFM69_REG_13_OCP, RF_OCP_ON | RF_OCP_TRIM_95);
    }
    // Return Transceiver to original mode
    setMode(oldMode);
}

void RFM69::SetLnaMode(uint8_t lnaMode) {
	// RF_TESTLNA_NORMAL (default)
	// RF_TESTLNA_SENSITIVE
	spiWrite(RFM69_REG_58_TEST_LNA, lnaMode);
}

void RFM69::clearFifo() {
	// Must only be called in RX Mode
	// Apparently this works... found in HopeRF demo code
	setMode(RFM69_MODE_STDBY);
	setMode(RFM69_MODE_RX);
}

float RFM69::readTemp()
{
    // Store current transceiver mode
	uint8_t oldMode = _mode;
	// Set mode into Standby (required for temperature measurement)
	setMode(RFM69_MODE_STDBY);
	
	// Trigger Temperature Measurement
	spiWrite(RFM69_REG_4E_TEMP1, RF_TEMP1_MEAS_START);
	// Check Temperature Measurement has started
	if(!(RF_TEMP1_MEAS_RUNNING && spiRead(RFM69_REG_4E_TEMP1))){
		return 255.0;
	}
	// Wait for Measurement to complete
	while(RF_TEMP1_MEAS_RUNNING && spiRead(RFM69_REG_4E_TEMP1)) { };
	// Read raw ADC value
	uint8_t rawTemp = spiRead(RFM69_REG_4F_TEMP2);
	
	// Set transceiver back to original mode
	setMode(oldMode);
	// Return processed temperature value
	return (159+_temperatureFudge)-float(rawTemp);
}

int RFM69::lastRssi() {
    // If called straight after an RX, will return RSSI of the RX (sampled during preamble)
	return _lastRssi;
}

int RFM69::sampleRssi() {
    // Must only be called in RX mode
	if(_mode!=RFM69_MODE_RX) {
	    // Not sure what happens otherwise, so make sure
		return 0;
	}
	// Trigger RSSI Measurement
	spiWrite(RFM69_REG_23_RSSI_CONFIG, RF_RSSI_START);
	// Wait for Measurement to complete
	while(!(RF_RSSI_DONE && spiRead(RFM69_REG_23_RSSI_CONFIG))) { };
	// Read, store in _lastRssi and return RSSI Value
	_lastRssi = -(spiRead(RFM69_REG_24_RSSI_VALUE)/2);
	return _lastRssi;
}