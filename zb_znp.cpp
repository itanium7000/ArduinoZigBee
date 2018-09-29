#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "zb_znp.h"
#include <Arduino.h>

#define RX_BUFFER_SIZE		256

SoftwareSerial znp_serial(2, 3);

uint8_t zb_znp::get_sequence_send() {
	if (sequence_send == 0xFF) {
		sequence_send = 0;
	} else {
		sequence_send++;
	}
	return sequence_send;
}

int update_read_len;
uint8_t zb_znp::update() {
	update_read_len = znp_serial.available();
	if (update_read_len) {
		for (int i = 0; i < update_read_len; i++) {
			znp_buf[i] = znp_serial.read();
		}
		znp_frame_parser(znp_buf, update_read_len);
	}
}

int zb_znp::write(uint8_t* data, uint32_t len) {
	for (int i = 0; i < len; i++) {
		znp_serial.write(*(data + i));
	}
	return len;
}

int zb_znp::read(uint8_t* data, uint32_t len) {
	uint32_t counter = 40; //time out waiting for message is 4s.

	while(znp_serial.available() <= 0) {
		delay(100);
		if (counter-- == 0) {
			break;
		}
	}

	int r_len = znp_serial.available();
	int r_len_sum = 0;
	int r_c;

	if (r_len > len) {
		return -1;
	}

	while (r_len > 0) {
		for (int i = r_len_sum; i < (r_len_sum + r_len); i++) {
			r_c = (int)znp_serial.read();
			if (r_c >= 0) {
				*(data + i) = (uint8_t)r_c;
			}
		}

		r_len_sum += r_len;
		r_len = znp_serial.available();
	}

	return r_len_sum;
}

uint8_t zb_znp::set_tc_require_key_exchange(uint8_t bdb_trust_center_require_key_exchange) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(APP_CNF_BDB_SET_TC_REQUIRE_KEY_EXCHANGE);
	i++;
	znp_buf[i] = LSB(APP_CNF_BDB_SET_TC_REQUIRE_KEY_EXCHANGE);
	i++;
	znp_buf[i] = bdb_trust_center_require_key_exchange;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(APP_CNF_BDB_SET_TC_REQUIRE_KEY_EXCHANGE_SRSP);
}

uint8_t zb_znp::znp_frame_parser(uint8_t* data, uint32_t len) {
	uint8_t ch, ret = ZNP_PARSER_IDLE;
	int rx_remain;

	while (len) {
		ch = *data++;
		len--;

		switch (znp_frame.state) {
		case SOP_STATE:
			if (SOF_CHAR == ch) {
				znp_frame.state = LEN_STATE;
				ret = ZNP_PARSER_DOING;
			}
			break;

		case LEN_STATE:
			ret = ZNP_PARSER_DOING;
			znp_frame.zigbee_msg.len = ch;
			znp_frame.zigbee_msg_data_index = 0;
			znp_frame.state = CMD0_STATE;
			break;

		case CMD0_STATE:
			ret = ZNP_PARSER_DOING;
			znp_frame.zigbee_msg.cmd0 = ch;
			znp_frame.state = CMD1_STATE;
			break;

		case CMD1_STATE:
			ret = ZNP_PARSER_DOING;
			znp_frame.zigbee_msg.cmd1 = ch;
			if (znp_frame.zigbee_msg.len) {
				znp_frame.state = DATA_STATE;
			} else {
				znp_frame.state = FCS_STATE;
			}
			break;

		case DATA_STATE: {
			ret = ZNP_PARSER_DOING;
			znp_frame.zigbee_msg.data[znp_frame.zigbee_msg_data_index++] = ch;

			rx_remain = znp_frame.zigbee_msg.len - znp_frame.zigbee_msg_data_index;

			if (len >= rx_remain) {
				memcpy((uint8_t*) (znp_frame.zigbee_msg.data + znp_frame.zigbee_msg_data_index), data, rx_remain);
				znp_frame.zigbee_msg_data_index += rx_remain;
				len -= rx_remain;
				data += rx_remain;
			} else {
				memcpy((uint8_t*) (znp_frame.zigbee_msg.data + znp_frame.zigbee_msg_data_index), data, len);
				znp_frame.zigbee_msg_data_index += len;
				len = 0;
			}

			if (znp_frame.zigbee_msg.len == znp_frame.zigbee_msg_data_index) {
				znp_frame.state = FCS_STATE;
			}
		}
			break;

		case FCS_STATE: {
			znp_frame.state = SOP_STATE;

			znp_frame.frame_fcs = ch;

			if (znp_frame.frame_fcs == znp_frame_calc_fcs(znp_frame.zigbee_msg)) {

				if (!znp_frame.zigbee_msg_denied_handle) {
					zigbee_message_handler(znp_frame.zigbee_msg);
				}

				ret = ZNP_PARSER_DONE;
			} else {
				/* TODO: handle checksum incorrect */
				ret = ZNP_PARSER_ERR;
			}
		}
			break;

		default:
			break;
		}
	}

	return ret;
}

uint8_t zb_znp::znp_frame_calc_fcs(zigbee_msg_t& zigbee_msg) {
	uint8_t i;
	uint8_t xorResult;
	xorResult = zigbee_msg.len ^ zigbee_msg.cmd0 ^ zigbee_msg.cmd1;
	for (i = 0; i < zigbee_msg.len; i++)
		xorResult = xorResult ^ zigbee_msg.data[i];
	return (xorResult);
}

uint8_t zb_znp::waiting_for_message(uint16_t cmd) {
	uint8_t rx_buffer[RX_BUFFER_SIZE];
	int rx_read_len;
	int retry_time = 10;
	znp_frame.zigbee_msg_denied_handle = 1;
	while (retry_time > 0) {
		rx_read_len = read(rx_buffer, RX_BUFFER_SIZE);
		if (znp_frame_parser(rx_buffer, rx_read_len) == ZNP_PARSER_DONE) {
			if (BUILD_UINT16(znp_frame.zigbee_msg.cmd1, znp_frame.zigbee_msg.cmd0) == cmd) {
				znp_frame.zigbee_msg_denied_handle = 0;
				return znp_frame.zigbee_msg.data[0];
			}
		}
		retry_time--;
	}
	znp_frame.zigbee_msg_denied_handle = 0;
	return ZNP_NOT_SUCCESS;
}

uint8_t zb_znp::waiting_for_status(uint16_t cmd, uint8_t status) {
	uint8_t rx_buffer[RX_BUFFER_SIZE];
	int rx_read_len;
	int retry_time = 10;
	znp_frame.zigbee_msg_denied_handle = 1;
	while (retry_time > 0) {
		rx_read_len = read(rx_buffer, RX_BUFFER_SIZE);
		if (znp_frame_parser(rx_buffer, rx_read_len) == ZNP_PARSER_DONE) {
			if (BUILD_UINT16(znp_frame.zigbee_msg.cmd1, znp_frame.zigbee_msg.cmd0) == cmd &&
					znp_frame.zigbee_msg.data[0] == status) {
				znp_frame.zigbee_msg_denied_handle = 0;
				return znp_frame.zigbee_msg.data[0];
			}
		}
		retry_time--;
	}
	znp_frame.zigbee_msg_denied_handle = 0;
	return ZNP_NOT_SUCCESS;
}

uint8_t zb_znp::get_msg_return(uint16_t cmd, uint8_t status, uint8_t* rx_buffer, uint32_t* len) {
	int rx_read_len;
	int retry_time = 10;
	znp_frame.zigbee_msg_denied_handle = 1;
	while (retry_time > 0) {
		rx_read_len = read(rx_buffer, RX_BUFFER_SIZE);
		if (znp_frame_parser(rx_buffer, rx_read_len) == ZNP_PARSER_DONE) {
			if (BUILD_UINT16(znp_frame.zigbee_msg.cmd1, znp_frame.zigbee_msg.cmd0) == cmd &&
					znp_frame.zigbee_msg.data[0] == status) {
				memcpy(rx_buffer, znp_frame.zigbee_msg.data, znp_frame.zigbee_msg.len);
				*len = znp_frame.zigbee_msg.len;

				znp_frame.zigbee_msg_denied_handle = 0;
				return ZNP_SUCCESS;
			}
		}
		retry_time--;
	}
	znp_frame.zigbee_msg_denied_handle = 0;
	return ZNP_NOT_SUCCESS;
}

uint8_t zb_znp::calc_fcs(uint8_t* msg, uint8_t len) {
	uint8_t result = 0;
	while (len--) {
		result ^= *msg++;
	}
	return result;
}

uint8_t zb_znp::app_cnf_set_allowrejoin_tc_policy(uint8_t mode) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(APP_CNF_SET_ALLOWREJOIN_TC_POLICY);
	i++;
	znp_buf[i] = LSB(APP_CNF_SET_ALLOWREJOIN_TC_POLICY);
	i++;
	znp_buf[i] = mode;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;
	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(APP_CNF_SET_ALLOWREJOIN_TC_POLICY | 0x6000);
}

#define HARD_RESET				0x00
#define SOFT_RESET				0x01
uint8_t zb_znp::soft_reset() {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(SYS_RESET_REQ);
	i++;
	znp_buf[i] = LSB(SYS_RESET_REQ);
	i++;
	znp_buf[i] = SOFT_RESET;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}
	return waiting_for_message(SYS_RESET_IND);
}

uint8_t zb_znp::set_startup_options(uint8_t opt) {
	int8_t i = 0;
	if (opt > (STARTOPT_CLEAR_CONFIG + STARTOPT_CLEAR_STATE)) {
		return ZNP_NOT_SUCCESS;
	}

	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = LSB(ZB_WRITE_CONFIGURATION);
	i++;

	znp_buf[i] = ZCD_NV_STARTUP_OPTION;
	i++;
	znp_buf[i] = ZCD_NV_STARTUP_OPTION_LEN;
	i++;
	znp_buf[i] = opt;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(ZB_WRITE_CONFIGURATION | 0x6000);
}

uint8_t zb_znp::set_panid(uint16_t pan_id) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = LSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = ZCD_NV_PANID;
	i++;
	znp_buf[i] = ZCD_NV_PANID_LEN;
	i++;
	znp_buf[i] = LSB(pan_id);
	i++;
	znp_buf[i] = MSB(pan_id);
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;
	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}
	return waiting_for_message(ZB_WRITE_CONFIGURATION | 0x6000);
}

uint8_t zb_znp::set_zigbee_device_type(uint8_t dev_type) {
	uint8_t i = 0;
	if (dev_type > END_DEVICE) {
		return ZNP_NOT_SUCCESS;
	}
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = LSB(ZB_WRITE_CONFIGURATION);
	i++;

	znp_buf[i] = ZCD_NV_LOGICAL_TYPE;
	i++;
	znp_buf[i] = ZCD_NV_LOGICAL_TYPE_LEN;
	i++;
	znp_buf[i] = dev_type;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;
	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(ZB_WRITE_CONFIGURATION | 0x6000);
}

uint8_t zb_znp::set_transmit_power(uint8_t tx_power_db) {
	uint8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(SYS_SET_TX_POWER);
	i++;
	znp_buf[i] = LSB(SYS_SET_TX_POWER);
	i++;
	znp_buf[i] = tx_power_db;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;
	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	uint8_t znpresult = waiting_for_message(SYS_SET_TX_POWER | 0x6000);
	if (znpresult == tx_power_db) {
		return ZNP_SUCCESS;
	}
	return ZNP_NOT_SUCCESS;
}

uint8_t zb_znp::set_channel_mask(uint8_t primary, uint32_t channelmask) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(APP_CNF_BDB_SET_CHANNEL);
	i++;
	znp_buf[i] = LSB(APP_CNF_BDB_SET_CHANNEL);
	i++;

	znp_buf[i] = primary;
	i++;
	znp_buf[i] = LSB(channelmask);
	i++;
	znp_buf[i] = (channelmask & 0xFF00) >> 8;
	i++;
	znp_buf[i] = (channelmask & 0xFF0000) >> 16;
	i++;
	znp_buf[i] = channelmask >> 24;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(APP_CNF_BDB_SET_CHANNEL | 0x6000);
}

uint8_t zb_znp::af_register_generic_application(uint8_t endpoint) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(AF_REGISTER);
	i++;
	znp_buf[i] = LSB(AF_REGISTER);
	i++;
	znp_buf[i] = endpoint;
	i++;
	znp_buf[i] = LSB(DEFAULT_PROFILE_ID);
	i++;
	znp_buf[i] = MSB(DEFAULT_PROFILE_ID);
	i++;
	znp_buf[i] = LSB(DEVICE_ID);
	i++;
	znp_buf[i] = MSB(DEVICE_ID);
	i++;
	znp_buf[i] = DEVICE_VERSION;
	i++;
	znp_buf[i] = LATENCY_NORMAL;
	i++;
	znp_buf[i] = 0;
	i++;				// number of binding input clusters
	znp_buf[i] = 0;
	i++;				// number of binding output clusters
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(AF_REGISTER | 0x6000);
}

uint8_t zb_znp::zb_app_register_request() {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_APP_REGISTER_REQUEST);
	i++;
	znp_buf[i] = LSB(ZB_APP_REGISTER_REQUEST);
	i++;
	znp_buf[i] = DEFAULT_ENDPOINT;
	i++;
	znp_buf[i] = LSB(DEFAULT_PROFILE_ID);
	i++;
	znp_buf[i] = MSB(DEFAULT_PROFILE_ID);
	i++;
	znp_buf[i] = LSB(DEVICE_ID);
	i++;
	znp_buf[i] = MSB(DEVICE_ID);
	i++;
	znp_buf[i] = DEVICE_VERSION;
	i++;
	znp_buf[i] = LATENCY_NORMAL;
	i++;
	znp_buf[i] = 0;
	i++;				// number of binding input clusters
	znp_buf[i] = 0;
	i++;				// number of binding output clusters

	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(ZB_APP_REGISTER_REQUEST | 0x6000);
}

uint8_t zb_znp::zb_start_request() {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_START_REQUEST);
	i++;
	znp_buf[i] = LSB(ZB_START_REQUEST);
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(ZB_START_CONFIRM);
}

uint8_t zb_znp::zdo_start_application() {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZDO_STARTUP_FROM_APP);
	i++;
	znp_buf[i] = LSB(ZDO_STARTUP_FROM_APP);
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}
	return waiting_for_status(ZDO_STATE_CHANGE_IND, 0x09);
}

uint8_t zb_znp::set_callbacks(uint8_t cb) {
	if ((cb != CALLBACKS_ENABLED) && (cb != CALLBACKS_DISABLED)) {
		return ZNP_NOT_SUCCESS;
	}
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = LSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = ZCD_NV_ZDO_DIRECT_CB;
	i++;
	znp_buf[i] = ZCD_NV_ZDO_DIRECT_CB_LEN;
	i++;
	znp_buf[i] = cb;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}
	return waiting_for_message(ZB_WRITE_CONFIGURATION | 0x6000);
}

uint8_t  zb_znp::start_coordinator(uint8_t opt) {
	uint8_t znpResult;
	uint8_t rx_buffer[RX_BUFFER_SIZE];
	int rx_read_len;

	/* setup uart interface */
	znp_serial.begin(115200);

	//Serial.print("start_coordinator\n");
	znpResult = soft_reset();
	if (znpResult == ZNP_NOT_SUCCESS) {
		//Serial.print("ERROR: reset ZNP \n");
		return znpResult;
	}

	znpResult = zb_read_configuration(ZCD_NV_PANID, rx_buffer, (uint32_t*) &rx_read_len);

	if (rx_buffer[3] == 0xFF && rx_buffer[4] == 0xFF) {
		opt = 1;
	}

	if (opt == 0) {
		//Serial.print("Skipping startup option !\n");
		for (int i = 1; i < 0x20; i++) {
			if (i == 0x01 || i == 0x0A) {
				znpResult = af_register_generic_application(i);
				if (znpResult != ZNP_SUCCESS) {
					//Serial.print("ERROR: af register ");
					//Serial.print(i);
					//Serial.print("\n");
					return znpResult;
				}
			}
		}
		znpResult = bdb_start_commissioning(COMMISSIONING_MODE_STEERING, 1, 0);		// 0x02 is Network Steering
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: Network Steering \n");
			return znpResult;
		}

	} else {
		znpResult = set_startup_options(DEFAULT_STARTUP_OPTIONS);
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: startup option. \n");
			return znpResult;
		}
		//Serial.println("set_startup_options");

		znpResult = soft_reset();
		if (znpResult == ZNP_NOT_SUCCESS) {
			//Serial.print("ERROR: reset ZNP \n");
			return znpResult;
		}
		//Serial.println("soft_reset");

		znpResult = set_panid((uint16_t) PAN_ID);
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: PAN ID \n");
			return znpResult;
		}
		//Serial.println("set_panid");

		znpResult = set_zigbee_device_type(COORDINATOR);
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: Device type \n");
			return znpResult;
		}
		//Serial.println("set_zigbee_device_type");

		//Set primary channel mask & disable secondary channel mask
		znpResult = set_channel_mask(CHANNEL_TRUE, (uint32_t) DEFAULT_CHANNEL_MASK);
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: set channel mask \n");
			return znpResult;
		}
		//Serial.println("set_channel_mask");

		znpResult = set_channel_mask(CHANNEL_FALSE, (uint32_t) 0);
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: set channel mask \n");
			return znpResult;
		}
		//Serial.println("set_channel_mask");

		znpResult = app_cnf_set_allowrejoin_tc_policy(1);
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: set allow join \n");
			return znpResult;
		}
		//Serial.println("app_cnf_set_allowrejoin_tc_policy");

		znpResult = set_transmit_power(DEFAULT_TX_POWER);
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: set transmit power \n");
			return znpResult;
		}
		//Serial.println("set_transmit_power");
		// Set ZCD_NV_ZDO_DIRECT_CB
		znpResult = set_callbacks(CALLBACKS_ENABLED);
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: set callback \n");
			return znpResult;
		}
		//Serial.println("set_callbacks");

		for (int i = 1; i < 0x20; i++) {
			if (i == 0x01 || i == 0x0A) {
				znpResult = af_register_generic_application(i);
				if (znpResult != ZNP_SUCCESS) {
					//Serial.print("ERROR: af register ");
					//Serial.print(i);
					//Serial.print("\n");
					return znpResult;
				}
			}
		}
		//Serial.println("af_register_generic_application");
		// Start commissioning using network formation as parameter to start coordinator
		znpResult = bdb_start_commissioning(COMMISSIONING_MODE_INFORMATION, 2, 0);		// 0x04 is Network Formation
		if (znpResult != ZNP_SUCCESS) {
			//Serial.print("ERROR: Network Steering \n");
			return znpResult;
		}
		//Serial.println("bdb_start_commissioning");
	}

	znpResult = set_tc_require_key_exchange(0);
	if (znpResult != ZNP_SUCCESS) {
		//Serial.print("ERROR: set TC key exchange \n");
		return znpResult;
	}
	//Serial.println("set_tc_require_key_exchange");

	znpResult = set_permit_joining_req(SHORT_ADDRESS_COORDINATOR, DISABLE_PERMIT_JOIN, 0);
	if (znpResult != ZNP_SUCCESS) {
		//Serial.print("ERROR: disabled permit join \n");
		return znpResult;
	}
	//Serial.println("set_permit_joining_req");

	/*get MAC address of coordinator*/
	get_mac_addr_req(SHORT_ADDRESS_COORDINATOR, 0x00, 0x00);
	//Serial.println("get_mac_addr_req");

	return ZNP_SUCCESS;
}

uint8_t zb_znp::bdb_start_commissioning(uint8_t mode_config, uint8_t mode_receiving, uint8_t flag_waiting) {
	(void)mode_receiving;
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(APP_CNF_BDB_START_COMMISSIONING);
	i++;
	znp_buf[i] = LSB(APP_CNF_BDB_START_COMMISSIONING);
	i++;
	znp_buf[i] = mode_config;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	if (flag_waiting == 0) {
		return waiting_for_status(APP_CNF_BDB_COMMISSIONING_NOTIFICATION, ZNP_SUCCESS);
	}
	return ZNP_SUCCESS;
}

uint8_t zb_znp::util_get_device_info() {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(UTIL_GET_DEVICE_INFO);
	i++;
	znp_buf[i] = LSB(UTIL_GET_DEVICE_INFO);
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return ZNP_SUCCESS;
}

uint8_t zb_znp::zdo_mgmt_leave_req(uint16_t short_add, uint8_t ieee_addr[8], uint8_t flags) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZDO_MGMT_LEAVE_REQ);
	i++;
	znp_buf[i] = LSB(ZDO_MGMT_LEAVE_REQ);
	i++;
	znp_buf[i] = LSB(short_add);
	i++;
	znp_buf[i] = MSB(short_add);
	i++;
	memcpy((uint8_t*) &znp_buf[i], ieee_addr, 8);
	i += 8;
	znp_buf[i] = flags;
	i++;

	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return ZNP_SUCCESS;
}

uint8_t zb_znp::set_permit_joining_req(uint16_t short_addr, uint8_t timeout, uint8_t flag_waiting) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_PERMIT_JOINING_REQUEST);
	i++;
	znp_buf[i] = LSB(ZB_PERMIT_JOINING_REQUEST);
	i++;
	znp_buf[i] = LSB(short_addr);
	i++;
	znp_buf[i] = MSB(short_addr);
	i++;
	znp_buf[i] = timeout;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	if (flag_waiting == 0) {
		return waiting_for_message(ZDO_MGMT_PERMIT_JOIN_RSP);
	}

	return ZNP_SUCCESS;
}

uint8_t zb_znp::get_mac_addr_req(uint16_t short_addr, uint8_t req_type, uint8_t start_index) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZDO_IEEE_ADDR_REQ);
	i++;
	znp_buf[i] = LSB(ZDO_IEEE_ADDR_REQ);
	i++;
	znp_buf[i] = LSB(short_addr);
	i++;
	znp_buf[i] = MSB(short_addr);
	i++;
	znp_buf[i] = req_type;
	i++;
	znp_buf[i] = start_index;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return ZNP_SUCCESS;
}

uint8_t zb_znp::send_af_data_req(af_data_request_t af_data_request) {
	int32_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(AF_DATA_REQUEST);
	i++;
	znp_buf[i] = LSB(AF_DATA_REQUEST);
	i++;
	znp_buf[i] = LSB(af_data_request.dst_address);
	i++;
	znp_buf[i] = MSB(af_data_request.dst_address);
	i++;
	znp_buf[i] = af_data_request.dst_endpoint;
	i++;
	znp_buf[i] = af_data_request.src_endpoint;
	i++;
	znp_buf[i] = MSB(af_data_request.cluster_id);
	i++;
	znp_buf[i] = LSB(af_data_request.cluster_id);
	i++;
	znp_buf[i] = af_data_request.trans_id;
	i++;
	znp_buf[i] = af_data_request.options;
	i++;
	znp_buf[i] = af_data_request.radius;
	i++;
	znp_buf[i] = af_data_request.len;
	i++;
	memcpy((uint8_t*) &znp_buf[i], af_data_request.data, af_data_request.len);
	if (znp_buf[i] & 0x04) {
		znp_buf[i + 3] = get_sequence_send();
	} else {
		znp_buf[i + 1] = get_sequence_send();
	}
	i += af_data_request.len;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return ZNP_SUCCESS;
}

uint8_t zb_znp::set_security_mode(uint8_t security_mode) {
	if (security_mode > SECURITY_MODE_COORD_DIST_KEYS) {
		return ZNP_NOT_SUCCESS;
	}
	int32_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = LSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = ZCD_NV_SECURITY_MODE;
	i++;
	znp_buf[i] = ZCD_NV_SECURITY_MODE_LEN;
	i++;
	znp_buf[i] = (security_mode > 0);
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	if (waiting_for_message(ZB_WRITE_CONFIGURATION | 0x6000) == ZNP_NOT_SUCCESS) {
		return ZNP_NOT_SUCCESS;
	}

	if (security_mode != SECURITY_MODE_OFF) {
		i = 0;
		znp_buf[i] = ZNP_SOF;
		i++;
		znp_buf[i] = 0;
		i++;
		znp_buf[i] = MSB(ZB_WRITE_CONFIGURATION);
		i++;
		znp_buf[i] = LSB(ZB_WRITE_CONFIGURATION);
		i++;
		znp_buf[i] = ZCD_NV_PRECFGKEYS_ENABLE;
		i++;
		znp_buf[i] = ZCD_NV_PRECFGKEYS_ENABLE_LEN;
		i++;
		znp_buf[i] = (security_mode == SECURITY_MODE_PRECONFIGURED_KEYS);
		i++;
		znp_buf[1] = i - 4;
		znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
		i++;

		if (write(znp_buf, i) < 0) {
			return ZNP_NOT_SUCCESS;
		}

		return waiting_for_message(ZB_WRITE_CONFIGURATION | 0x6000);
	}
	return ZNP_SUCCESS;
}

uint8_t zb_znp::set_security_key(uint8_t* key) {
	int32_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = LSB(ZB_WRITE_CONFIGURATION);
	i++;
	znp_buf[i] = ZCD_NV_PRECFGKEY;
	i++;
	znp_buf[i] = ZCD_NV_PRECFGKEY_LEN;
	i++;
	memcpy((uint8_t*) &znp_buf[i], key, ZCD_NV_PRECFGKEY_LEN);
	i += ZCD_NV_PRECFGKEY_LEN;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return waiting_for_message(ZB_WRITE_CONFIGURATION | 0x6000);
}

uint8_t zb_znp::zb_get_device_info(uint8_t param, uint8_t* rx_buffer, uint32_t* len) {
	uint32_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_GET_DEVICE_INFO);
	i++;
	znp_buf[i] = LSB(ZB_GET_DEVICE_INFO);
	i++;
	znp_buf[i] = param;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return get_msg_return((ZB_GET_DEVICE_INFO | 0x6000), param, rx_buffer, len);
}

uint8_t zb_znp::zb_read_configuration(uint8_t config_id, uint8_t* rx_buffer, uint32_t* len) {
	int8_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZB_READ_CONFIGURATION);
	i++;
	znp_buf[i] = LSB(ZB_READ_CONFIGURATION);
	i++;
	znp_buf[i] = config_id;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;
	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return get_msg_return(ZB_READ_CONFIGURATION_RSP, ZNP_SUCCESS, rx_buffer, len);
}

uint8_t zb_znp::zdo_binding_req(binding_req_t binding_req) {
	int32_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZDO_BIND_REQ);
	i++;
	znp_buf[i] = LSB(ZDO_BIND_REQ);
	i++;
	znp_buf[i] = binding_req.src_short_addr[0];
	i++;
	znp_buf[i] = binding_req.src_short_addr[1];
	i++;
	memcpy((uint8_t*) &znp_buf[i], (uint8_t*) binding_req.src_ieee_addr, 8);
	i += 8;
	znp_buf[i] = binding_req.src_endpoint;
	i++;
	znp_buf[i] = LSB(binding_req.cluster_id);
	i++;
	znp_buf[i] = MSB(binding_req.cluster_id);
	i++;
	znp_buf[i] = binding_req.dst_mode;
	i++;
	if (binding_req.dst_mode == ADDRESS_64_BIT) {
		memcpy((uint8_t*) &znp_buf[i], (uint8_t*) binding_req.dst_address, 8);
		i += 8;
	} else {
		memcpy((uint8_t*) &znp_buf[i], (uint8_t*) binding_req.dst_address, 2);
		i += 2;
	}
	znp_buf[i] = binding_req.dst_endpoint;
	i++;

	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return ZNP_SUCCESS;
}

uint8_t zb_znp::zdo_simple_desc_req(uint16_t dst_addr, uint8_t dst_enpoint) {
	int32_t i = 0;
	znp_buf[i] = ZNP_SOF;
	i++;
	znp_buf[i] = 0;
	i++;
	znp_buf[i] = MSB(ZDO_SIMPLE_DESC_REQ);
	i++;
	znp_buf[i] = LSB(ZDO_SIMPLE_DESC_REQ);
	i++;
	znp_buf[i] = LSB(dst_addr); //dst_addr
	i++;
	znp_buf[i] = MSB(dst_addr); // dst_addr
	i++;
	znp_buf[i] = LSB(dst_addr); // NWKAddrOfInterest
	i++;
	znp_buf[i] = MSB(dst_addr); //NWKAddrOfInterest
	i++;
	znp_buf[i] = dst_enpoint;
	i++;
	znp_buf[1] = i - 4;
	znp_buf[i] = calc_fcs((uint8_t *) &znp_buf[1], (i - 1));
	i++;

	if (write(znp_buf, i) < 0) {
		return ZNP_NOT_SUCCESS;
	}

	return ZNP_SUCCESS;
}
