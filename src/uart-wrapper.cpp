/* Pan Tilt Zoom UART wrapper class
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 *
 * Wrapper around the UART IO implementation. Uses the serial_cpp
 * library on the back end to find the serial port devices, and manage
 * the port settings (baud rate, etc). Also manages error conditions and
 * reconnects the port if it gets hot unplugged & replugged.
 */

#include "uart-wrapper.hpp"
#include "ptz-device.hpp"

/* Matches the positive values of Qt's former QSerialPort::BaudRate enum,
 * which the "baud_rate" properties dropdown used to be populated from. */
static const int standard_baud_rates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};

static const int reconnect_poll_interval_ms = 2000;

PTZUARTWrapper::PTZUARTWrapper(QString &port_name)
{
	uart.setPort(port_name.toStdString());
	serial_cpp::Timeout timeout = serial_cpp::Timeout::simpleTimeout(100);
	uart.setTimeout(timeout);
	connect(&reconnect_timer, &QTimer::timeout, this, &PTZUARTWrapper::open);
}

PTZUARTWrapper::~PTZUARTWrapper()
{
	close();
}

bool PTZUARTWrapper::open()
{
	try {
		uart.open();
	} catch (const std::exception &e) {
		if (!open_failure_logged) {
			blog(LOG_INFO, "Unable to open UART %s: %s", qPrintable(portName()), e.what());
			open_failure_logged = true;
		}
		reconnect_timer.start(reconnect_poll_interval_ms);
		return false;
	}
	open_failure_logged = false;
	blog(LOG_INFO, "UART %s connected", qPrintable(portName()));

	/* Connected - stop retrying until we lose the port again; reconnect() restarts it. */
	reconnect_timer.stop();

	/* Start the dedicated reader thread */
	stop_reader = false;
	uint64_t my_generation = ++generation;
	reader_thread = QThread::create([this, my_generation]() {
		uint8_t buf[256];
		while (!stop_reader.load()) {
			try {
				size_t n = uart.read(buf, sizeof(buf));
				if (n > 0) {
					QByteArray data(reinterpret_cast<const char *>(buf), (int)n);
					QMetaObject::invokeMethod(
						this, [this, data]() { receiveBytes(data); }, Qt::QueuedConnection);
				}
			} catch (const std::exception &e) {
				blog(LOG_INFO, "UART %s disappeared: %s", qPrintable(portName()), e.what());
				break;
			}
		}
		/* Check the exit condition. If it wasn't a request to
		 * stop, then kick the reconnect method in the main thread */
		if (!stop_reader.load()) {
			QMetaObject::invokeMethod(
				this,
				[this, my_generation]() {
					if (generation == my_generation)
						reconnect();
				},
				Qt::QueuedConnection);
		}
	});
	reader_thread->start();

	return true;
}

void PTZUARTWrapper::close()
{
	if (reader_thread) {
		stop_reader = true;
		reader_thread->wait();
		delete reader_thread;
		reader_thread = nullptr;
	}
	if (uart.isOpen()) {
		try {
			uart.close();
		} catch (const std::exception &e) {
			blog(LOG_INFO, "Error closing UART %s: %s", qPrintable(portName()), e.what());
		}
	}
}

void PTZUARTWrapper::reconnect()
{
	close();
	/* Not open (whether we just closed it or it already wasn't) - resume
	 * retrying open() on a timer. */
	reconnect_timer.start(reconnect_poll_interval_ms);
}

void PTZUARTWrapper::setBaudRate(int baudRate)
{
	if (!baudRate || (uint32_t)baudRate == uart.getBaudrate())
		return;

	uart.setBaudrate((uint32_t)baudRate);
}

int PTZUARTWrapper::baudRate() const
{
	return (int)uart.getBaudrate();
}

void PTZUARTWrapper::setConfig(OBSData config)
{
	setBaudRate((int)obs_data_get_int(config, "baud_rate"));
}

void PTZUARTWrapper::save(OBSData config) const
{
	obs_data_set_string(config, "port", qPrintable(portName()));
	obs_data_set_int(config, "baud_rate", baudRate());
}

void PTZUARTWrapper::addOBSProperties(obs_properties_t *props)
{
	obs_property_t *p;

	p = obs_properties_add_list(props, "port", obs_module_text("PTZ.Device.SerialPort"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	for (const auto &port : serial_cpp::list_ports())
		obs_property_list_add_string(p, port.port.c_str(), port.port.c_str());

	p = obs_properties_add_list(props, "baud_rate", obs_module_text("PTZ.Device.SerialBaud"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	for (int rate : standard_baud_rates) {
		auto rate_string = std::to_string(rate);
		obs_property_list_add_int(p, rate_string.c_str(), rate);
	}
}

void PTZUARTWrapper::send(const QByteArray &packet)
{
	if (!uart.isOpen())
		return;
	try {
		uart.write(reinterpret_cast<const uint8_t *>(packet.constData()), (size_t)packet.size());
	} catch (const std::exception &e) {
		blog(LOG_INFO, "Error writing to UART %s: %s", qPrintable(portName()), e.what());
		reconnect();
	}
}
