/*******************************************************************************
 * Copyright (c) 2014, 2015 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Allan Stockdill-Mander/Ian Craggs - initial API and implementation and/or initial documentation
 *******************************************************************************/
#include "MQTTClient.h"

static void NewMessageData(MessageData* md, MQTTString* aTopicName, MQTTMessage* aMessage) {
    md->topicName = aTopicName;
    md->message = aMessage;
}


static int getNextPacketId(MQTTClient *c) {
    return c->next_packetid = (c->next_packetid == MAX_PACKET_ID) ? 1 : c->next_packetid + 1;
}


static int sendPacket(MQTTClient* c, int length, Timer* timer)
{
    int rc = MQTT_FAILURE, 
        sent = 0;

    while (sent < length && !platform_timer_isexpired(timer))
    {
        rc = platform_network_write(c->ipstack, &c->buf[sent], length, platform_timer_left(timer));
        if (rc < 0)  // there was an error writing the data
            break;
        sent += rc;
    }

    if (sent == length)
    {
        platform_timer_countdown(&c->ping_timer, c->keepAliveInterval*1000); // record the fact that we have successfully sent the packet
        rc = MQTT_SUCCESS;
    }
    else
    {
        rc = MQTT_CONNECTION_LOST;
    }
    return rc;
}


void MQTTClientInit(MQTTClient* c, Network* network, unsigned int command_timeout_ms,
		unsigned char* sendbuf, size_t sendbuf_size, unsigned char* readbuf, size_t readbuf_size)
{
    c->ipstack = network;
    
    c->command_timeout_ms = command_timeout_ms;
    c->buf = sendbuf;
    c->buf_size = sendbuf_size;
    c->readbuf = readbuf;
    c->readbuf_size = readbuf_size;
    c->isconnected = 0;
    c->ping_outstanding = 0;
    c->messageHandler = 0;
    c->messageHandlerData = 0;
	c->next_packetid = 1;

    platform_timer_init(&c->ping_timer);
    platform_timer_init(&c->pingresp_timer);
	platform_mutex_init(&c->mutex);
}


void MQTTClientDeinit(MQTTClient *c)
{
    if (!c) return;
    platform_timer_deinit(&c->ping_timer);
    platform_timer_deinit(&c->pingresp_timer);
    platform_mutex_deinit(&c->mutex);
}


static int decodePacket(MQTTClient* c, int* value, int timeout)
{
    unsigned char i;
    int multiplier = 1;
    int len = 0;
    const int MAX_NO_OF_REMAINING_LENGTH_BYTES = 4;

    *value = 0;
    do
    {
        int rc = MQTTPACKET_READ_ERROR;

        if (++len > MAX_NO_OF_REMAINING_LENGTH_BYTES)
        {
            rc = MQTTPACKET_READ_ERROR; /* bad data */
            goto exit;
        }
        rc = platform_network_read(c->ipstack, &i, 1, timeout);
        if (rc != 1)
            goto exit;
        *value += (i & 127) * multiplier;
        multiplier *= 128;
    } while ((i & 128) != 0);
exit:
    return len;
}


static int readPacket(MQTTClient* c, Timer* timer)
{
    int rc = MQTT_FAILURE;
    MQTTHeader header = {0};
    int len = 0;
    int rem_len = 0;

    /* 1. read the header byte.  This has the packet type in it */
	int read_bytes = platform_network_read(c->ipstack, c->readbuf, 1, platform_timer_left(timer));

	if (read_bytes != 1) 
	{
		if (read_bytes == 0) 
			rc = MQTT_CONNECTION_LOST;
        goto exit;
	}

    len = 1;
    /* 2. read the remaining length.  This is variable in itself */
    decodePacket(c, &rem_len, platform_timer_left(timer));
    len += MQTTPacket_encode(c->readbuf + 1, rem_len); /* put the original remaining length back into the buffer */

    /* 3. read the rest of the buffer using a callback to supply the rest of the data */
    if (rem_len > 0 && (platform_network_read(c->ipstack, c->readbuf + len, rem_len, platform_timer_left(timer)) != rem_len))
        goto exit;

    header.byte = c->readbuf[0];
    rc = header.bits.type;
exit:
    return rc;
}


// assume topic filter and name is in correct format
// # can only be at end
// + and # can only be next to separator
char MQTTisTopicMatched(char* topicFilter, MQTTString* topicName)
{
    char* curf = topicFilter;
    char* curn = topicName->lenstring.data;
    char* curn_end = curn + topicName->lenstring.len;
    
    while (*curf && curn < curn_end)
    {
        if (*curn == '/' && *curf != '/')
            break;
        if (*curf != '+' && *curf != '#' && *curf != *curn)
            break;
        if (*curf == '+')
        {   // skip until we meet the next separator, or end of string
            char* nextpos = curn + 1;
            while (nextpos < curn_end && *nextpos != '/')
                nextpos = ++curn + 1;
        }
        else if (*curf == '#')
            curn = curn_end - 1;    // skip until end of string
        curf++;
        curn++;
    };
    
    return (curn == curn_end) && (*curf == '\0');
}


int deliverMessage(MQTTClient* c, MQTTString* topicName, MQTTMessage* message)
{
    int rc = MQTT_FAILURE;

    if (c->messageHandler != NULL) 
    {
        MessageData md;
        NewMessageData(&md, topicName, message);
        c->messageHandler(&md, c->messageHandlerData);
        rc = MQTT_SUCCESS;
    }   
    
    return rc;
}


int keepalive(MQTTClient* c)
{
    int rc = MQTT_FAILURE;

    if (c->keepAliveInterval == 0 || !c->isconnected)
    {
        rc = MQTT_SUCCESS;
        goto exit;
    }

    //printf("%s: %d, ping time left: %d, tick count = %u\n", __func__, __LINE__, TimerLeftMS(&c->ping_timer), xTaskGetTickCount());

    if (platform_timer_isexpired(&c->ping_timer))
    {
        if (!c->ping_outstanding)
        {
            Timer timer;
            platform_timer_init(&timer);
            platform_timer_countdown(&timer, 1000);
            int len = MQTTSerialize_pingreq(c->buf, c->buf_size);
            if (len > 0 && (rc = sendPacket(c, len, &timer)) == MQTT_SUCCESS) // send the ping packet
            {
                platform_timer_countdown(&c->pingresp_timer, c->command_timeout_ms);
                c->ping_outstanding = 1;
                platform_printf("sent ping request\n");
            }

            if (len > 0 && rc != MQTT_SUCCESS)
            {
                platform_printf("%s: %d failed to send ping request, rc = %d\n", __func__, __LINE__, rc);
            }
        }
    }

exit:
    return rc;
}


int cycle(MQTTClient* c, Timer* timer)
{
    Timer t;
    platform_timer_init(&t);

    int len = 0, packet_type, rc = MQTT_SUCCESS;

    // read the socket, see what work is due
    if ((packet_type = readPacket(c, timer)) == MQTT_CONNECTION_LOST)
	{
		rc = MQTT_CONNECTION_LOST;
		goto exit;
	}
    
    switch (packet_type)
    {
        case CONNACK:
        case PUBACK:
        case SUBACK:
            break;
        case PUBLISH:
        {
            MQTTString topicName;
            MQTTMessage msg = {0};
            int intQoS;
            if (MQTTDeserialize_publish(&msg.dup, &intQoS, &msg.retained, &msg.id, &topicName,
               (unsigned char**)&msg.payload, &msg.payloadlen, c->readbuf, c->readbuf_size) != 1)
                goto exit;
            msg.qos = (enum QoS)intQoS;
            deliverMessage(c, &topicName, &msg);
            if (msg.qos != QOS0)
            {
                if (msg.qos == QOS1)
                    len = MQTTSerialize_ack(c->buf, c->buf_size, PUBACK, 0, msg.id);
                else if (msg.qos == QOS2)
                    len = MQTTSerialize_ack(c->buf, c->buf_size, PUBREC, 0, msg.id);
                if (len <= 0)
                    rc = MQTT_FAILURE;
                else
                {
                    platform_timer_countdown(&t, c->command_timeout_ms);
                    rc = sendPacket(c, len, &t);
                }
                if (rc == MQTT_FAILURE)
                    goto exit; // there was a problem
            }
            break;
        }
        case PUBREC:
        {
            unsigned short mypacketid;
            unsigned char dup, type;
            platform_timer_countdown(&t, c->command_timeout_ms);
            if (MQTTDeserialize_ack(&type, &dup, &mypacketid, c->readbuf, c->readbuf_size) != 1)
                rc = MQTT_FAILURE;
            else if ((len = MQTTSerialize_ack(c->buf, c->buf_size, PUBREL, 0, mypacketid)) <= 0)
                rc = MQTT_FAILURE;
            else if ((rc = sendPacket(c, len, &t)) != MQTT_SUCCESS) // send the PUBREL packet
                rc = MQTT_FAILURE; // there was a problem
            if (rc == MQTT_FAILURE)
                goto exit; // there was a problem
            break;
        }
        case PUBCOMP:
            break;
        case PINGRESP:
            c->ping_outstanding = 0;
            platform_printf("received ping response\n");
            break;
    }

    keepalive(c);

    if (c->ping_outstanding && platform_timer_isexpired(&c->pingresp_timer))
    {
        c->ping_outstanding = 0;
		platform_printf("ping response was not received within keepalive timeout of %d\n", 
                c->keepAliveInterval);
        rc = MQTT_CONNECTION_LOST;
    }

exit:
	if (rc == MQTT_SUCCESS)
		rc = packet_type;

	return rc;
}


int MQTTYield(MQTTClient* c, int timeout_ms)
{
    int rc = MQTT_SUCCESS;
    Timer timer;
    platform_timer_init(&timer);
    platform_timer_countdown(&timer, timeout_ms);

    if (!c->isconnected)
        return rc;

	do
    {
        platform_mutex_lock(&c->mutex);

        rc = cycle(c, &timer);

        platform_mutex_unlock(&c->mutex);

        if (rc != MQTT_SUCCESS)
            break;

	} while (!platform_timer_isexpired(&timer));
        
    return rc;
}


int waitfor(MQTTClient* c, int packet_type, Timer* timer)
{
    int rc = MQTT_FAILURE;

    do
    {
        if (platform_timer_isexpired(timer))
        {
            break; // we timed out
        }

        rc = cycle(c, timer);

        if (rc == MQTT_CONNECTION_LOST)
            break;
    }
    while (rc != packet_type);  
    
    return rc;
}


int MQTTConnect(MQTTClient* c, MQTTPacket_connectData* options)
{
    Timer connect_timer;
    int rc = MQTT_FAILURE;
    MQTTPacket_connectData default_options = MQTTPacket_connectData_initializer;
    int len = 0;

	platform_mutex_lock(&c->mutex);
	if (c->isconnected) /* don't send connect packet again if we are already connected */
		goto exit;
    
    platform_timer_init(&connect_timer);
    platform_timer_countdown(&connect_timer, c->command_timeout_ms);

    if (options == 0)
        options = &default_options; /* set default options if none were supplied */
    
    c->ping_outstanding = 0;
    c->keepAliveInterval = options->keepAliveInterval;
    platform_timer_countdown(&c->ping_timer, c->keepAliveInterval*1000);

    if ((len = MQTTSerialize_connect(c->buf, c->buf_size, options)) <= 0)
        goto exit;
    if ((rc = sendPacket(c, len, &connect_timer)) != MQTT_SUCCESS)  // send the connect packet
        goto exit; // there was a problem
    
    // this will be a blocking call, wait for the connack
    if (waitfor(c, CONNACK, &connect_timer) == CONNACK)
    {
        unsigned char connack_rc = 255;
        unsigned char sessionPresent = 0;
        if (MQTTDeserialize_connack(&sessionPresent, &connack_rc, c->readbuf, c->readbuf_size) == 1)
            rc = connack_rc;
        else
            rc = MQTT_FAILURE;
    }
    else
        rc = MQTT_FAILURE;
    
exit:
    if (rc == MQTT_SUCCESS)
        c->isconnected = 1;

	platform_mutex_unlock(&c->mutex);

    return rc;
}


int MQTTisConnected(MQTTClient* c)
{
    int ret = 0;

	platform_mutex_lock(&c->mutex);

    if (c)
        ret = c->isconnected;

	platform_mutex_unlock(&c->mutex);

    return ret;
}


int MQTTSubscribe(MQTTClient* c, const char* topicFilter, enum QoS qos)
{ 
    int rc = MQTT_FAILURE;  
    Timer timer;
    int len = 0;
    MQTTString topic = MQTTString_initializer;
    topic.cstring = (char *)topicFilter;
    
	platform_mutex_lock(&c->mutex);

	if (!c->isconnected)
		goto exit;

    platform_timer_init(&timer);
    platform_timer_countdown(&timer, c->command_timeout_ms);
    
    len = MQTTSerialize_subscribe(c->buf, c->buf_size, 0, getNextPacketId(c), 1, &topic, (int*)&qos);
    if (len <= 0)
        goto exit;
    if ((rc = sendPacket(c, len, &timer)) != MQTT_SUCCESS) // send the subscribe packet
        goto exit;             // there was a problem

    if (waitfor(c, SUBACK, &timer) == SUBACK)      // wait for suback 
    {
        int count = 0, grantedQoS = -1;
        unsigned short mypacketid;
        if (MQTTDeserialize_suback(&mypacketid, 1, &count, &grantedQoS, c->readbuf, c->readbuf_size) == 1)
            rc = grantedQoS; // 0, 1, 2 or 0x80 
    }
    else 
    {
		rc = MQTT_CONNECTION_LOST;
	}
        
exit:
	platform_mutex_unlock(&c->mutex);
    return rc;
}


int MQTTUnsubscribe(MQTTClient* c, const char* topicFilter)
{   
    int rc = MQTT_FAILURE;
    Timer timer;    
    MQTTString topic = MQTTString_initializer;
    topic.cstring = (char *)topicFilter;
    int len = 0;

	platform_mutex_lock(&c->mutex);
	if (!c->isconnected)
		goto exit;

    platform_timer_init(&timer);
    platform_timer_countdown(&timer, c->command_timeout_ms);
    
    if ((len = MQTTSerialize_unsubscribe(c->buf, c->buf_size, 0, getNextPacketId(c), 1, &topic)) <= 0)
        goto exit;
    if ((rc = sendPacket(c, len, &timer)) != MQTT_SUCCESS) // send the subscribe packet
        goto exit; // there was a problem
    
    if (waitfor(c, UNSUBACK, &timer) == UNSUBACK)
    {
        unsigned short mypacketid;  // should be the same as the packetid above
        if (MQTTDeserialize_unsuback(&mypacketid, c->readbuf, c->readbuf_size) == 1)
            rc = 0; 
    }
    else 
    {
        rc = MQTT_CONNECTION_LOST;
	}
    
exit:
	platform_mutex_unlock(&c->mutex);
    return rc;
}


int MQTTPublish(MQTTClient* c, const char* topicName, MQTTMessage* message)
{
    int rc = MQTT_FAILURE;
    Timer timer;   
    MQTTString topic = MQTTString_initializer;
    topic.cstring = (char *)topicName;
    int len = 0;

	platform_mutex_lock(&c->mutex);
	if (!c->isconnected)
		goto exit;

    platform_timer_init(&timer);
    platform_timer_countdown(&timer, c->command_timeout_ms);

    if (message->qos == QOS1 || message->qos == QOS2)
        message->id = getNextPacketId(c);
    
    len = MQTTSerialize_publish(c->buf, c->buf_size, 0, message->qos, message->retained, message->id, 
              topic, (unsigned char*)message->payload, message->payloadlen);
    if (len <= 0)
        goto exit;
    if ((rc = sendPacket(c, len, &timer)) != MQTT_SUCCESS) // send the subscribe packet
    {
        goto exit; // there was a problem
    }

    if (message->qos == QOS1)
    {
        if (waitfor(c, PUBACK, &timer) == PUBACK)
        {
            unsigned short mypacketid;
            unsigned char dup, type;
            if (MQTTDeserialize_ack(&type, &dup, &mypacketid, c->readbuf, c->readbuf_size) != 1)
            {
                platform_printf("failed to deserialize ACK\n");
                rc = MQTT_FAILURE;
            }
        }
        else
        {
            rc = MQTT_CONNECTION_LOST;
        }
    }
    else if (message->qos == QOS2)
    {
        if (waitfor(c, PUBCOMP, &timer) == PUBCOMP)
        {
            unsigned short mypacketid;
            unsigned char dup, type;
            if (MQTTDeserialize_ack(&type, &dup, &mypacketid, c->readbuf, c->readbuf_size) != 1)
                rc = MQTT_FAILURE;
        }
        else 
        {
            rc = MQTT_CONNECTION_LOST;
		}
    }
    
exit:
	platform_mutex_unlock(&c->mutex);
    return rc;
}


int MQTTDisconnect(MQTTClient* c)
{  
    int rc = MQTT_FAILURE;
    Timer timer;     // we might wait for incomplete incoming publishes to complete
    int len = 0;

	platform_mutex_lock(&c->mutex);
    platform_timer_init(&timer);
    platform_timer_countdown(&timer, c->command_timeout_ms);

	len = MQTTSerialize_disconnect(c->buf, c->buf_size);
    if (len > 0)
        rc = sendPacket(c, len, &timer);            // send the disconnect packet
        
    c->isconnected = 0;

	platform_mutex_unlock(&c->mutex);
    return rc;
}

