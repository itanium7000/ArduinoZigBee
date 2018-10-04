#include <zb_znp.h>
#include <zb_zcl.h>

#define DBG_ZB_FRAME

zb_znp zigbee_network;

int zb_znp::zigbee_message_handler(zigbee_msg_t& zigbee_msg) {
	/* zigbee start debug message */
	Serial.print("[ZB msg] len: ");
	Serial.print(zigbee_msg.len);
	Serial.print(" cmd0: ");
	Serial.print(zigbee_msg.cmd0, HEX);
	Serial.print(" cmd1: ");
	Serial.print(zigbee_msg.cmd1, HEX);
	Serial.print(" data: ");
	for (int i = 0; i < zigbee_msg.len; i++) {
		Serial.print(zigbee_msg.data[i], HEX);
		Serial.print(" ");
	}
	Serial.println("");
	/* zigbee stop debug message */

	uint16_t zigbee_cmd = BUILD_UINT16(zigbee_msg.cmd1, zigbee_msg.cmd0);

	switch(zigbee_cmd) {
	case ZDO_MGMT_LEAVE_REQ: {
		Serial.println("ZDO_MGMT_LEAVE_REQ");
	}
		break;

	case ZB_RECEIVE_DATA_INDICATION: {
		Serial.println("ZB_RECEIVE_DATA_INDICATION");
	}
		break;

	case AF_INCOMING_MSG: {
		af_incoming_msg_t* st_af_incoming_msg = (af_incoming_msg_t*)zigbee_msg.data;
		Serial.println("AF_INCOMING_MSG");

#if defined (DBG_ZB_FRAME)
		char buf[9];
		char buf1[18];
		Serial.print("group_id: ");
		sprintf(buf, "%04x", st_af_incoming_msg->group_id);
		Serial.println(buf);

		Serial.print("cluster_id: ");
		sprintf(buf, "%04x", st_af_incoming_msg->cluster_id);
		Serial.println(buf);

		Serial.print("src_addr: ");
		sprintf(buf, "%04x", st_af_incoming_msg->src_addr);
		Serial.println(buf);

		Serial.print("src_endpoint: ");
		Serial.println(st_af_incoming_msg->src_endpoint, HEX);

		Serial.print("dst_endpoint: ");
		Serial.println(st_af_incoming_msg->dst_endpoint, HEX);

		Serial.print("was_broadcast: ");
		Serial.println(st_af_incoming_msg->was_broadcast, HEX);

		Serial.print("link_quality: ");
		Serial.println(st_af_incoming_msg->link_quality, HEX);

		Serial.print("security_use: ");
		Serial.println(st_af_incoming_msg->security_use, HEX);

		Serial.print("time_stamp: ");
		sprintf(buf1, "%08x", st_af_incoming_msg->time_stamp);
		Serial.println(buf1);

		Serial.print("trans_seq_num: ");
		Serial.println(st_af_incoming_msg->trans_seq_num, HEX);

		Serial.print("len: ");
		Serial.println(st_af_incoming_msg->len, HEX);

		Serial.print("data: ");
		for (int i = 0 ; i < st_af_incoming_msg->len ; i++) {
			Serial.print(st_af_incoming_msg->payload[i], HEX);
			Serial.print(" ");
		}
		Serial.println(" ");
#endif
	}
		break;

	case ZDO_MGMT_LEAVE_RSP: {
		Serial.println("ZDO_MGMT_LEAVE_RSP");
	}
		break;
	}
}

void setup() {
	Serial.begin(115200);

	/* Khởi động router */
	Serial.println("\nstart_router");
	if (zigbee_network.start_router() == 0) {
		Serial.println("start router successfully");
	}
	else {
		Serial.println("start router error");
	}
}

/* ký tự tạm để xử lý yêu cầu từ terminal */
char serial_cmd;

void loop() {
	/* hàm update() phải được gọi trong vòng lặp để xử lý các gói tin nhận được từ ZigBee Shield */
	zigbee_network.update();

	/* Kiểm tra / thực hiện các lệnh từ terminal */
	if (Serial.available()) {
		serial_cmd = Serial.read();

		switch(serial_cmd) {
			/* Gửi request cho router join vào coordinator */
		case '1': {
			Serial.println("bdb_start_commissioning");
			zigbee_network.bdb_start_commissioning(COMMISSIONING_MODE_STEERING, 1, 1);
		}
			break;

			/* Gửi data từ router đến coordinator */
		case '2': {
			Serial.println("send_af_data_req\n");

			uint8_t st_buffer[3] = { 0x01, 0x02, 0x03}; // data muốn gửi đi

			af_data_request_t st_af_data_request;
			st_af_data_request.cluster_id    = 0x0000;
			st_af_data_request.dst_address   = 0x0000;
			st_af_data_request.dst_endpoint  = 0X01;
			st_af_data_request.src_endpoint  = 0X01;
			st_af_data_request.trans_id      = 0x00;
			st_af_data_request.options       = 0X10;
			st_af_data_request.radius        = 0x0F;
			st_af_data_request.len           = sizeof(st_buffer);
			st_af_data_request.data          = st_buffer;

			zigbee_network.send_af_data_req(st_af_data_request);
		}
			break;

			/******************************************************************
			 *  Ví dụ:
			 * gửi data từ Router đến Gateway (coodinator)
			 * các thông số cần thiết cho quá trình này bao gồm
			 * 2. độ dài của mảng data cần truyền
			 * 3. data

		case 's': {
			uint8_t st_buffer[10];
			af_data_request_t st_af_data_request;
			st_af_data_request.cluster_id    = 0x0000;
			st_af_data_request.dst_address   = 0x0000; // Địa chỉ của coodinator luôn là 0x0000
			st_af_data_request.dst_endpoint  = 0x01;
			st_af_data_request.src_endpoint  = 0x01;
			st_af_data_request.trans_id      = 0x00;
			st_af_data_request.options       = 0x10;
			st_af_data_request.radius        = 0x0F;
			st_af_data_request.len           = [ Độ dài data cần gửi đi ] ví dụ: sizeof(st_buffer)
			st_af_data_request.data          = [ data ] ví dụ: st_buffer
			zigbee_network.send_af_data_req(st_af_data_request);
		}
			break;
			********************************************************************/

		default:
			break;
		}
	}
}
