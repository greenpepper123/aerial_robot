/*
 * servo.cpp
 *
 *  Created on: 2016/10/28
 *      Author: anzai
 *  Maintainer: bakui chou(2016/11/20)
 */

#include "servo.h"

void Servo::init(UART_HandleTypeDef* huart)
{
	servo_handler_.init(huart);
}

void Servo::update()
{
	servo_handler_.update();
}

void Servo::sendData()
{
	for (unsigned int i = 0; i < servo_handler_.getServoNum(); i++) {
		const ServoData& s = servo_handler_.getServo()[i];
		if (s.send_data_flag_ != 0) {
			CANServoData data(std::min(std::max((int)(s.present_position_ + s.overflow_offset_value_), 0), 4095),
							  s.present_temp_,
							  s.moving_,
							  s.present_current_,
							  s.hardware_error_status_);
			setMessage(CAN::MESSAGEID_SEND_SERVO_LIST[i], m_slave_id, 8, reinterpret_cast<uint8_t*>(&data));
			sendMessage(1);
		}
	}
}

void Servo::receiveDataCallback(uint8_t message_id, uint32_t DLC, uint8_t* data)
{
	switch (message_id) {
		case CAN::MESSAGEID_RECEIVE_SERVO_ANGLE:
		{
			for (unsigned int i = 0; i < servo_handler_.getServoNum(); i++) {
				ServoData& s = servo_handler_.getServo()[i];
				s.goal_position_ = (((data[i * 2 + 1]  & 0x0F ) << 8) | data[i * 2]) - s.overflow_offset_value_;
				bool torque_enable = (((data[i * 2 + 1] >> 7) & 0x01) != 0) ? true : false;
				if (s.torque_enable_ != torque_enable) {
					s.torque_enable_ = torque_enable;
					servo_handler_.setTorque(i);
				}
			}
			break;
		}
	}
}
