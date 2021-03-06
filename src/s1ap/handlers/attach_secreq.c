/*
 * Copyright (c) 2003-2018, Great Software Laboratory Pvt. Ltd.
 * Copyright (c) 2017 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "err_codes.h"
#include "message_queues.h"
#include "ipc_api.h"
#include "log.h"
#include "main.h"
#include "s1ap.h"
#include "sctp_conn.h"
#include "stage3_info.h"
#include "snow_3g.h"

/****Globals and externs ***/

/*Making global just to avoid stack passing*/
static char buf[S1AP_SECREQ_STAGE3_BUF_SIZE];

struct sec_mode_Q_msg *g_secReqInfo;

static Buffer g_sec_buffer;
static Buffer g_sec_value_buffer;
static Buffer g_sec_nas_buffer;

extern int g_enb_fd;
extern ipc_handle ipcHndl_smc;

extern struct time_stat g_attach_stats[];
/****Global and externs end***/

/**
Initialize the stage settings, Q,
destination communication etc.
*/
static void
init_stage()
{
	if ((ipcHndl_smc  = open_ipc_channel(
			S1AP_SECREQ_STAGE3_QUEUE, IPC_READ)) == -1) {
				log_msg(LOG_ERROR, "Error in opening reader for secreq IPC"
				"channel : %s\n", S1AP_SECREQ_STAGE3_QUEUE);
		pthread_exit(NULL);
	}
	log_msg(LOG_INFO, "secreq reader: Connected.\n");

	return;
}

/**
* Read next message from stage Q for processing.
*/
static int
read_next_msg()
{
	int bytes_read=0;

	memset(buf, 0, S1AP_SECREQ_STAGE3_BUF_SIZE);
	while (bytes_read < S1AP_SECREQ_STAGE3_BUF_SIZE) {//TODO : Recheck condition
		if ((bytes_read = read_ipc_channel(
				ipcHndl_smc, buf,
				S1AP_SECREQ_STAGE3_BUF_SIZE)) == -1) {
					log_msg(LOG_ERROR, "Error in reading \n");
					/* TODO : Add proper error handling */
				}
		log_msg(LOG_INFO, "secreq Message Received - len %d", bytes_read);
	}

	return bytes_read;
}

/**
* Get ProtocolIE value for Sec Request sent by mme-app
*/
static int
get_secreq_protoie_value(struct proto_IE *value)
{
	value->no_of_IEs = SEC_MODE_NO_OF_IES;

	value->data = (proto_IEs *) malloc(SEC_MODE_NO_OF_IES *
			sizeof(proto_IEs));

	value->data[0].mme_ue_s1ap_id = g_secReqInfo->ue_idx;
	value->data[1].enb_ue_s1ap_id = g_secReqInfo->enb_s1ap_ue_id;

	value->data[2].nas.header.security_header_type =
			IntegrityProtectedEPSSecCntxt;

	value->data[2].nas.header.proto_discriminator =
			EPSMobilityManagementMessages;

	/* placeholder for mac. mac value will be calculated later */
	uint8_t mac[MAC_SIZE] = {0};
	memcpy(value->data[2].nas.header.mac, mac, MAC_SIZE);

	value->data[2].nas.header.seq_no = g_secReqInfo->dl_seq_no;

	value->data[2].nas.header.message_type = SecurityModeCommand;

	value->data[2].nas.header.security_encryption_algo = Algo_EEA0;

	value->data[2].nas.header.security_integrity_algo = Algo_128EIA1;

	/* Security Param (1 octet) =
	 * Spare half octet, Type of Security, NAS KSI
	 * TODO: Remove hard coded value
	 */
	value->data[2].nas.header.nas_security_param = AUTHREQ_NAS_SECURITY_PARAM;

	value->data[2].nas.elements_len = SEC_MODE_NO_OF_NAS_IES;

	value->data[2].nas.elements = (nas_pdu_elements *)
			malloc(SEC_MODE_NO_OF_NAS_IES * sizeof(nas_pdu_elements));

	value->data[2].nas.elements->ue_network.len =
			g_secReqInfo->ue_network.len;

	memcpy(value->data[2].nas.elements->ue_network.capab,
			g_secReqInfo->ue_network.capab,
			g_secReqInfo->ue_network.len);

	return SUCCESS;
}


/**
* Stage specific message processing.
*/
static int
secreq_processing()
{
	unsigned char tmpStr[4];
	struct s1ap_PDU s1apPDU;
	uint8_t mac_data_pos;

	g_secReqInfo = (struct sec_mode_Q_msg *) buf;

	s1apPDU.procedurecode = id_downlinkNASTransport;
	s1apPDU.criticality = CRITICALITY_IGNORE;

	get_secreq_protoie_value(&s1apPDU.value);

	/* Copy values to g_sec_nas_buffer */

	/* id-NAS-PDU */
	g_sec_nas_buffer.pos = 0;
	nasPDU nas = s1apPDU.value.data[2].nas;

	unsigned char value = (nas.header.security_header_type << 4 |
			nas.header.proto_discriminator);

	buffer_copy(&g_sec_nas_buffer, &value, sizeof(value));

	/* placeholder for mac. mac value will be calculated later */
	buffer_copy(&g_sec_nas_buffer, &nas.header.mac, MAC_SIZE);

	mac_data_pos = g_sec_nas_buffer.pos;

	buffer_copy(&g_sec_nas_buffer, &nas.header.seq_no,
			sizeof(nas.header.seq_no));

	nas.header.security_header_type = Plain;
	value = nas.header.security_header_type |
				nas.header.proto_discriminator;
	buffer_copy(&g_sec_nas_buffer, &value, sizeof(value));

	buffer_copy(&g_sec_nas_buffer, &nas.header.message_type,
			sizeof(nas.header.message_type));

	value = (nas.header.security_encryption_algo << 4 |
				nas.header.security_integrity_algo);
	buffer_copy(&g_sec_nas_buffer, &value, sizeof(value));

	buffer_copy(&g_sec_nas_buffer, &nas.header.nas_security_param,
			sizeof(nas.header.nas_security_param));

	buffer_copy(&g_sec_nas_buffer, &nas.elements->ue_network.len,
			sizeof(nas.elements->ue_network.len));

	buffer_copy(&g_sec_nas_buffer, &nas.elements->ue_network.capab,
			nas.elements->ue_network.len);

	/* Calculate mac */
	uint8_t direction = 1;
	uint8_t bearer = 0;

	calculate_mac(g_secReqInfo->int_key, nas.header.seq_no,
			direction, bearer, &g_sec_nas_buffer.buf[mac_data_pos],
			g_sec_nas_buffer.pos - mac_data_pos,
			&g_sec_nas_buffer.buf[mac_data_pos - MAC_SIZE]);

	/* Copy values in g_sec_value_buffer */
	g_sec_value_buffer.pos = 0;

	/* TODO remove hardcoded values */
	char chProtoIENo[3] = {0,0,3};
	buffer_copy(&g_sec_value_buffer, chProtoIENo, 3);

	/* id-MME-UE-S1AP-ID */
	uint16_t protocolIe_Id = id_MME_UE_S1AP_ID;
	copyU16(tmpStr, protocolIe_Id);
	buffer_copy(&g_sec_value_buffer, tmpStr,
					sizeof(protocolIe_Id));

	unsigned char protocolIe_criticality = CRITICALITY_REJECT;
	buffer_copy(&g_sec_value_buffer, &protocolIe_criticality,
					sizeof(protocolIe_criticality));

	unsigned char datalen = 2;

	/* TODO need to add proper handling*/
	unsigned char mme_ue_id[3];
	datalen = copyU16(mme_ue_id, s1apPDU.value.data[0].mme_ue_s1ap_id);
	buffer_copy(&g_sec_value_buffer, &datalen, sizeof(datalen));
	buffer_copy(&g_sec_value_buffer, mme_ue_id, datalen);

	/* id-eNB-UE-S1AP-ID */
	protocolIe_Id = id_eNB_UE_S1AP_ID;
	copyU16(tmpStr, protocolIe_Id);
	buffer_copy(&g_sec_value_buffer, tmpStr,
						sizeof(protocolIe_Id));

	buffer_copy(&g_sec_value_buffer, &protocolIe_criticality,
					sizeof(protocolIe_criticality));

	/* TODO needs proper handling*/
	unsigned char enb_ue_id[3];
	datalen = copyU16(enb_ue_id, s1apPDU.value.data[1].enb_ue_s1ap_id);
	buffer_copy(&g_sec_value_buffer, &datalen, sizeof(datalen));
	buffer_copy(&g_sec_value_buffer, enb_ue_id, datalen);

	/* id-NAS-PDU */
	protocolIe_Id = id_NAS_PDU;
	copyU16(tmpStr, protocolIe_Id);
	buffer_copy(&g_sec_value_buffer, tmpStr,
			sizeof(protocolIe_Id));

	buffer_copy(&g_sec_value_buffer, &protocolIe_criticality,
			sizeof(protocolIe_criticality));


	datalen = g_sec_nas_buffer.pos + 1;
	buffer_copy(&g_sec_value_buffer, &datalen,
			sizeof(datalen));

	buffer_copy(&g_sec_value_buffer, &g_sec_nas_buffer.pos,
			sizeof(g_sec_nas_buffer.pos));

	buffer_copy(&g_sec_value_buffer, &g_sec_nas_buffer,
			g_sec_nas_buffer.pos);

	/* Copy values in g_sec_buffer */
	g_sec_buffer.pos = 0;

	unsigned char initiating_message = 0; /* TODO: Add enum */
	buffer_copy(&g_sec_buffer, &initiating_message,
			sizeof(initiating_message));

	buffer_copy(&g_sec_buffer, &s1apPDU.procedurecode,
			sizeof(s1apPDU.procedurecode));

	buffer_copy(&g_sec_buffer, &s1apPDU.criticality,
			sizeof(s1apPDU.criticality));

	buffer_copy(&g_sec_buffer, &g_sec_value_buffer.pos,
			sizeof(g_sec_value_buffer.pos));

	buffer_copy(&g_sec_buffer, &g_sec_value_buffer,
			g_sec_value_buffer.pos);

	free(s1apPDU.value.data[2].nas.elements);
	free(s1apPDU.value.data);
	//STIMER_GET_CURRENT_TP(g_attach_stats[s1apPDU.value.data[1].enb_ue_s1ap_id].secreq_out);
	return SUCCESS;
}

/**
* Post message to next handler of the stage
*/
static int
post_to_next()
{
	send_sctp_msg(g_secReqInfo->enb_fd, g_sec_buffer.buf, g_sec_buffer.pos);
	log_msg(LOG_INFO, "\n-----Stage3 completed.---\n");
	return SUCCESS;
}

/**
* Thread exit function for future reference.
*/
void
shutdown_secreqstage()
{
	close_ipc_channel(ipcHndl_smc);
	pthread_exit(NULL);
	return;
}

/**
* Thread function for stage.
*/
void*
secreq_handler(void *data)
{
	init_stage();
	log_msg(LOG_INFO, "SecReq handler ready.\n");

	while(1){
		read_next_msg();

		secreq_processing();

		post_to_next();
	}

	return NULL;
}
