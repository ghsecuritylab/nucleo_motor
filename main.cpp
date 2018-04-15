#include "intelliSense.h"	// for IntelliSense
#include <mbed.h>
#include <vector>
#include "source\EncodedMotor.h"	// motor encoder
#include "source\debugMonitor.h"	// enable LCD2004 as debug monitor
#include "source\TextLCD.h"
#include "source\MotorControl.h"
#include "mbed-os\rtos\Thread.h"
#include "mbed-os\rtos\EventFlags.h"

// set baudrate at mbed_config.h default 115200
// I2C scanner included, derived from Arduino I2C scanner
// http://www.gammon.com.au/forum/?id=10896

/////////////////////////////////
//// Declare connection//////////
/////////////////////////////////
// L298N Control Port
PinName MotorEnable = PA_15;			// connect ENA to PA_15
PinName MotorDirection1 = PA_14; // connect IN1 to PA_14
PinName MotorDirection2 = PA_13; // connect IN2 to PA_13
DigitalOut SolenoidEnable = PB_7;	// connect ENB to PB_7
// User Input Port
PinName WeldStartStop = PA_11; 		// connect WeldStartStop Btn to PA_11
PinName MotorStartStop = PC_13;		// connect MotorStartStop Btn to PA_12
PinName knob = PA_4;				// connect Potentionmeter to PA_4 (A2)

// LED Indicator Port
DigitalOut MotorOnLED = PB_15;		// connect MotorOnLED to PB_15
DigitalOut SolenoidOnLED = PB_14;	// connect SolenoidOnLED to PB_14
// Motor Encoder Port
PinName MotorEncoderA = PA_9;		// connect MotorEncoderA (blue) to PA_9 (D8)
PinName MotorEncoderB = PA_8;		// connect MotorEncoderB (purple) to PA_8 (D7)

// Specify PI control constant
const uint16_t Kp = 1; 
const uint16_t Ki = 0; 

//// Initiate object
EncodedMotor encoder(MotorEncoderA, MotorEncoderB, 1848*4, 25, EncodeType::X4);	// Encoded Motor object
RawSerial pc(SERIAL_TX, SERIAL_RX, 250000);							// serial communication protocol
MotorControl motor1(MotorEnable, MotorDirection1, MotorDirection2, &encoder);	// motor controller object 
DebugMonitor debugger(knob, &encoder, &pc);				// update status through LCD2004 and Serial Monitor
Timer timer; 

//// Declare interrupt
Ticker bridgeTicker;							// Periodic Interrupt for debugging purpose
Ticker motorBlinkLEDTicker;						// LED Blinker
InterruptIn motorInterrupt(MotorStartStop);		// Motor Button Interrupt
InterruptIn weldingInterrupt(WeldStartStop);	// Welding Button Interrupt
// InterruptIn steadyBtnInterrupt(PC_13);			// Steady State Button Interrupt (for debug)
DigitalOut steadyPin(PC_12);					// Steady State Changer
InterruptIn steadyInterrupt(PC_10);				// Steady State Change Interrupt

//// Declare thread
rtos::Thread motorLEDBlinking;				// Thread to perform LED Blinking
rtos::Thread statusUpdater;					// Thread to update status through Serial Monitor and LCD

//// Define constants
volatile bool weldSignal = false;			// Start welding flag
volatile bool motorStartSignal = false;		// Start motor flag
volatile bool motorSteadySignal = false;	// Motor in steady state flag
AnalogIn refSpeed(knob);					// Reference Speed from user through potentiometer
enum class motorState {
	Off,
	On,
	Unsteady
};

//// Define flags
rtos::EventFlags toPrintSignal;				// LCD signal flag

//// Define function
void I2C_scan()
{
	// Define I2C communication using port I2C1_SDA(D14, PB_9) and I2C1_SCL(D15, PB_8)
	I2C i2c(PB_9, PB_8); 
	// Define Serial Communication to Serial Monitor
	Serial pc(SERIAL_TX, SERIAL_RX);

	// Initializing I2C Scanner
	pc.printf("I2C Scanner initializing.... \n");
	uint8_t error, address;
	char mydata[2];
	mydata[1] = 0x00;
	std::vector<uint8_t> addrList;

	pc.printf("I2C Scanner start scanning. \n");

	// Start scanning using 8 bit address
	pc.printf("\n8-bit address.... \n");
	for (address = 1; address < 128; address++)
	{
		error = i2c.write(address, mydata, sizeof(mydata));					// error: 0 - success (ack), 1 - failure (nack)
		pc.printf("Address: %#X return %d\n", address, error);
		if (error == 0)
			addrList.push_back(address);	// add to addrList if address receive ack
	}
	for (auto &val : addrList)
	{
		pc.printf("Valid Address: %#X ", val); 		// print valid address
	}

	// Initialize and start scanning using 16-bit (8-bit shifted left) address
	addrList.clear();
	pc.printf("\n16-bit address.... \n");
	for (address = 1; address < 128; address++)
	{
		error = i2c.write((uint16_t)(address << 1), mydata, sizeof(mydata));  // error: 0 - success (ack), 1 - failure (nack)
		pc.printf("Address: %#X return %d\n", (uint16_t)(address << 1), error);
		if (error == 0)
			addrList.push_back(address);	// add to addrList if address receive ack
	}
	for (auto &val : addrList)
	{
		pc.printf("Valid Address: %#X ", val);		// print valid address
	}
}
void statusUpdate()
{
	while (1)
	{
		toPrintSignal.wait_all(1U);

		//// Output status
		debugger.printSignal();
		
		//// Output Flags to Serial monitor
		pc.printf("motorStartSignal: %d\n motorSteadySignal: %d\n weldSignal: %d\n SolenoidEnable = %d\n",
			motorStartSignal, motorSteadySignal, weldSignal, SolenoidEnable.read());
		pc.printf("Compensate: %f\n Speed: %f\n Error: %lf\n AdjError: %lf\n", 
			motor1.readComp(), motor1.readSpeed(), motor1.readError(), motor1.readAdjError());
	}
}

int main() {
	pc.printf("Initiating\n"); 
	// I2C Scanner.. comment out if not used... 
	// I2C_scan(); 
	
	// Initiate Interrupt and Ticker
	timer.start();
	steadyInterrupt.rise([]() {motorBlinkLEDTicker.detach(); MotorOnLED = 1; motorSteadySignal = 1; });				// LED when motorSteadySignal toHigh
	steadyInterrupt.fall([]() {motorBlinkLEDTicker.attach([]() 
		{motorStartSignal ?  MotorOnLED = !MotorOnLED : MotorOnLED = 0; motorSteadySignal = 0; }, 0.5f); });		// LED when motorSteadySignal toLow (to blink or not? )
	bridgeTicker.attach([]() {toPrintSignal.set(1U); }, 0.5f);														// Ticker for periodic interrupt
	motorInterrupt.rise([]() {motorStartSignal = !motorStartSignal; steadyPin = 1; steadyPin = 0; });				// Motor Button Intterupter
	weldingInterrupt.rise([&]() {
		bool toSolenoid = motorStartSignal && motorSteadySignal && !weldSignal;  
		toSolenoid ? weldSignal = true : weldSignal = false; });													// Welding Button Interrupter
		
	// Initiate Thread
	statusUpdater.start(&statusUpdate); 

	pc.printf("Ready\n"); 
	
	while (1) {
		// Active-Deactive Motor Rotation
		if (!motorStartSignal)
		{
			motor1.stop(); 
		}
		else
		{
			// motorpower(refSpeeWd.read(), &MotorEnable);	// Adjust motor signal... to be replace by PID 
			// motorpower(1, &MotorEnable);	// Adjust motor signal... to be replace by PID 

			// Set motorOnLEDState to blinking / solid light depends on motorSteadySignal 
			motorSteadySignal ? steadyPin = 1 : steadyPin = 0;
			motor1.run(0.7f); 
		}
			
		// Active-Deactive Solenoid for start welding
		SolenoidEnable = (int)weldSignal; 
		SolenoidOnLED = (int)weldSignal; 
	}
}