/*
 * Copyright (c) 2015 David Sunshine, <http://sunshin.es>
 * 
 * This file is part of melo.
 * 
 * melo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * melo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with melo.  If not, see <http://www.gnu.org/licenses/>.
 */

/******************************************************************************
*                                   Includes                                  *
******************************************************************************/
#include "melo.h"
#include "melo_priv.h"

/******************************************************************************
*                              Local Data Types                               *
******************************************************************************/
/*[[[cog
import cog
cog.outl("<%def name=\"build_func_name(x)\"><%return defaults['state_prefix'] + x + defaults['state_suffix']%></%def>")
cog.outl("<%include file=\"templates/types.tpl\" />")
]]]*/
/*[[[end]]]*/

/******************************************************************************
*                          Local Function Prototypes                          *
******************************************************************************/
static uint8_t  _get_event(void);
static void     _init_event_stack(void);
static void     _melo_create_cmd_byte(uint8_t * const b, const uint8_t cmd_type);
static void     _melo_create_r(uint8_t * const b);
static uint32_t _melo_esafe_uint(const uint8_t * const bytes, const uint8_t pe, const uint8_t he);
static void     _melo_frame_handler(const _m_frame * const frame, const bool crc_present);
static void     _melo_packet_handler(const _m_packet *packet);
static void     _melo_restore_r(uint8_t * const b);
static void     _melo_rx_byte(_m_frame_buffer * const frame_buffer, const uint8_t byte);
static void     _melo_serialize_frame(_m_frame_buffer * const frame_buffer);
static uint8_t  _melo_service_handler(const _m_packet * const packet);
static void     _notify_event(const uint8_t event);
static bool     _service_read_write(const _m_packet * const request, _m_packet * const response);
static bool     _service_NULL(const _m_packet * const request, _m_packet * const response);

/*[[[cog
import cog
cog.outl("<%include file=\"templates/prototypes.tpl\" />")
]]]*/
/*[[[end]]]*/

/******************************************************************************
*                               Local Variables                               *
******************************************************************************/
/*[[[cog
import cog
cog.outl("<%include file=\"templates/variables.tpl\" />")
]]]*/
/*[[[end]]]*/

static uint8_t recv_frame_buffer[MELO_MAX_FRAME_SIZE]        = {0};
static uint8_t send_frame_buffer[MELO_MAX_FRAME_SIZE]        = {0};
static uint8_t wait_frame_buffer[MELO_MAX_WAIT_FRAME_SIZE]   = {0};

static uint8_t recv_packet_buffer[MELO_CFG_MAX_DATA_LENGTH]  = {0};
static uint8_t send_packet_buffer[MELO_CFG_MAX_DATA_LENGTH]  = {0};
static uint8_t wait_packet_buffer;

static _m_frame_buffer wait_frame;
static _m_frame_buffer send_frame;
static _m_frame_buffer recv_frame;

static uint8_t  _m_event_stack_data[MELO_CFG_MAX_STACK_SIZE] = {0};
static MeloList _m_event_stack;

static const _m_service service_table[8] =
{
    /* 0 */ _service_read_write,
    /* 1 */ _service_NULL,
    /* 2 */ _service_NULL,
    /* 3 */ _service_NULL,
    /* 4 */ _service_NULL,
    /* 5 */ _service_NULL,
    /* 6 */ _service_NULL,
    /* 7 */ _service_NULL,
};

/******************************************************************************
*                        Exported Function Definitions                        *
******************************************************************************/
void MeloTransmitComplete(void)
{
    _notify_event(MELO_EVNET_TX_CONFIRMATION);
}

void MeloReceiveBytes(const uint8_t * const bytes, const uint8_t num)
{
    uint8_t i;

    for (i = 0; i < num; i++)
    {
        _melo_rx_byte( &recv_frame, bytes[i] );
    }
}

void MeloReceiveByte( const uint8_t byte )
{
    _melo_rx_byte( &recv_frame, byte );
}

uint8_t MeloServiceRequestBuilder(uint8_t * buffer, const uint8_t service, const uint8_t subfunction, const MeloList * const request_data, const bool use_crc)
{
    _m_frame_buffer tx_frame;
    
    tx_frame.frame.packet.command.raw_byte           = 0x00;
    tx_frame.frame.packet.command.fields.service     = service;
    tx_frame.frame.packet.command.fields.subfunction = subfunction;
    tx_frame.frame.packet.command.fields.status      = MELO_CMD_REQUEST_RESPONSE;
    tx_frame.frame.packet.byte_order                 = MELO_CFG_PE_ENDIANESS;
    
    tx_frame.buffer.data              = buffer;
    tx_frame.frame.packet.data.length = request_data->length;
    tx_frame.frame.packet.data.data   = request_data->data;
    tx_frame.crc_present              = use_crc;
    
    _melo_serialize_frame( &tx_frame );
    
    return tx_frame.buffer.length;
}

uint8_t MeloGetEndianess(void)
{
    /* MISRA deviation: MISRA 2012 Rule 1.3
       Reason: pointer casting is required to detect endianess at runtime.
    */
    const uint32_t         i = 1;
    const uint8_t  * const p = (const uint8_t  * const) &i;
    /* End of MISRA deviation */
    
    uint8_t   result;

    if (p[0] == 1)
    {
        result = MELO_LITTLE_ENDIAN;
    }
    else
    {
        result = MELO_BIG_ENDIAN;
    }
    
    return result;
}

void MeloInit(void)
{
    wait_frame.buffer.data = &wait_frame_buffer[0];
    wait_frame.buffer.size = MELO_MAX_WAIT_FRAME_SIZE;
    wait_frame.frame.packet.data.data = &wait_packet_buffer;
    wait_frame.frame.packet.data.size = MELO_WAIT_DATA_SIZE;

    send_frame.buffer.data = &send_frame_buffer[0];
    send_frame.buffer.size = MELO_MAX_FRAME_SIZE;
    send_frame.frame.packet.data.data = &send_packet_buffer[0];
    send_frame.frame.packet.data.size = MELO_CFG_MAX_DATA_LENGTH;

    recv_frame.buffer.data = &recv_frame_buffer[0];
    recv_frame.buffer.size = MELO_MAX_FRAME_SIZE;
    recv_frame.frame.packet.data.data = &recv_packet_buffer[0];
    recv_frame.frame.packet.data.size = MELO_CFG_MAX_DATA_LENGTH;

    _init_event_stack();
}

void MeloBackground(void)
{
    uint8_t event;

    for (event = _get_event(); event != MELO_EVENT_IDLE; event = _get_event())
    {
        _current_state = _table[_current_state].function(_STATE_ACTION_DURING, event);
    }
}

/******************************************************************************
*                          Local Function Definitions                         *
******************************************************************************/
static bool _service_NULL(const _m_packet * const request, _m_packet * const response)
{
    return false;
}

static void _notify_event(const uint8_t event)
{
    if (_m_event_stack.length == (_m_event_stack.size - 1))
    {
        /* Do nothing - the stack is full */
    }
    else
    {
        if (_m_event_stack.data[_m_event_stack.length] == event)
        {
            /* Do nothing - ignore duplicate events */
        }
        else
        {
            _m_event_stack.length++;
            _m_event_stack.data[_m_event_stack.length] = event;
        }
    }
}

static uint32_t _melo_esafe_uint(const uint8_t * const bytes, const uint8_t pe, const uint8_t he)
{
    uint32_t result = 0;

    if ( pe == he )
    {
        result |= ( ((uint32_t) bytes[0])                    );
        result |= ( ((uint32_t) bytes[1]) << ((uint8_t) 8u ) );
        result |= ( ((uint32_t) bytes[2]) << ((uint8_t) 16u) );
        result |= ( ((uint32_t) bytes[3]) << ((uint8_t) 24u) );
    }
    else
    {
        result |= ( ((uint32_t) bytes[3])                    );
        result |= ( ((uint32_t) bytes[2]) << ((uint8_t) 8u ) );
        result |= ( ((uint32_t) bytes[1]) << ((uint8_t) 16u) );
        result |= ( ((uint32_t) bytes[0]) << ((uint8_t) 24u) );
    }

    return result;
}

static bool _service_read_write(const _m_packet * const request, _m_packet * const response)
{
    bool result = true;
    const uint8_t * value;
    uint32_t address;
    uint8_t  index;

    if (request->command.fields.subfunction == 1u)
    {
        address = _melo_esafe_uint( &(request->data.data[0]), MELO_CFG_PE_ENDIANESS, request->byte_order );
        value   = MeloCreatePointer( address );
        
        response->data.length = 4u;
        
        for (index = 0; index < response->data.length; index++)
        {
            response->data.data[index] = value[index];
        }
    }
    else if (request->command.fields.subfunction == 2u)
    {
        address = _melo_esafe_uint( &(request->data.data[0]), MELO_CFG_PE_ENDIANESS, request->byte_order );
        value   = MeloCreatePointer( address );

        response->data.length  = 1u;
        response->data.data[0] = *value;
    }
    else
    {
        result = false;
    }

    return result;
}

static void _melo_create_r(uint8_t * const b)
{
	*b = ((*b & RESERVED_TX_HIGH_MASK) << 1u) | (*b & RESERVED_LOW_MASK);
}

static void _melo_restore_r(uint8_t * const b)
{
	*b = ((*b & RESERVED_RX_HIGH_MASK) >> 1u) | (*b & RESERVED_LOW_MASK);
}

static uint8_t _get_event(void)
{
    uint8_t element;

    if (_m_event_stack.length == 0)
    {
        element = MELO_EVENT_IDLE;
    }
    else
    {
        element = _m_event_stack.data[_m_event_stack.length];
        _m_event_stack.length--;
    }

    return element;
}

static void _init_event_stack(void)
{
    _m_event_stack.data   = &_m_event_stack_data[0];
    _m_event_stack.size   = MELO_CFG_MAX_STACK_SIZE;
    _m_event_stack.length = 0;
}

static void _melo_create_cmd_byte(uint8_t * const b, const uint8_t cmd_type)
{
    BIT_SET(*b, FRAME_RESERVED_BIT_POS);

    if (MELO_CFG_PE_ENDIANESS == MELO_BIG_ENDIAN)
    {
        BIT_SET(*b, FRAME_ENDIAN_BIT_POS);
    }
    else
    {
        BIT_CLEAR(*b, FRAME_ENDIAN_BIT_POS);
    }

    if (cmd_type == MELO_CMD_HEAD)
    {
        BIT_SET(*b, FRAME_MARKER_BIT_POS);
    }
    else
    {
        BIT_CLEAR(*b, FRAME_MARKER_BIT_POS);
    }
}

static void _melo_rx_byte(_m_frame_buffer * const frame_buffer, const uint8_t byte)
{
    uint8_t index = 0;

    if (IS_FRAME_CONTROL(byte) != false)
    {
        if (IS_FRAME_ESCAPED(byte) != false)
        {
            /* Load escape buffer */
            frame_buffer->escape_buffer = byte & ESCAPE_BYTE_MASK;
        }
        else if (IS_FRAME_HEAD(byte) != false)
        {
            /* Reset receive buffer */
            for (frame_buffer->buffer.length = 0;
                 frame_buffer->buffer.length < frame_buffer->buffer.size;
                 frame_buffer->buffer.length++)
            {
                frame_buffer->buffer.data[frame_buffer->buffer.length] = 0x00;
            }
            frame_buffer->buffer.length = 0;

            /* Load configuration values */
            frame_buffer->crc_present             = IS_FRAME_CRC_PRESENT(byte);
            frame_buffer->frame.packet.byte_order = GET_FRAME_ENDIANNESS(byte);
        }
        else if (IS_FRAME_TAIL(byte) != false)
        {
            /* Check consistency between HEAD and TAIL */
            if (
                 ( frame_buffer->crc_present             == IS_FRAME_CRC_PRESENT(byte) ) &&
                 ( frame_buffer->frame.packet.byte_order == GET_FRAME_ENDIANNESS(byte) )
               )
            {
                /* Process receive buffer */
                if (frame_buffer->crc_present != false)
                {
                    /* CRC precedes the TAIL by one byte */
                    frame_buffer->frame.crc = frame_buffer->buffer.data[frame_buffer->buffer.length - 1u];
                }
                else
                {
                    /* Do nothing - no CRC is present to process */
                    frame_buffer->frame.crc = 0x00;
                }

                /* Unpack the rest of the data */
                frame_buffer->frame.packet.data.length = frame_buffer->buffer.data[0];
                _melo_restore_r( &(frame_buffer->frame.packet.data.length) );

                frame_buffer->frame.packet.command.raw_byte = frame_buffer->buffer.data[1];

                for (index = 0; index < frame_buffer->frame.packet.data.length; index++)
                {
                    frame_buffer->frame.packet.data.data[index] = frame_buffer->buffer.data[2 + index];
                }

                /* Indicate a packet has been received */
                _notify_event(MELO_EVENT_REQUEST_RECEIVED);
            }
            else
            {
                /* Error - CRC/Endian mismatch between HEAD and TAIL */
            }
        }
        else
        {
            /* Error - Unknown type of frame! */
        }
    }
    else
    {
        /* Fill the buffer */
        /* TODO: NULL CHECK for: frame_buffer->buffer.data or InitComplete */
        frame_buffer->buffer.data[frame_buffer->buffer.length] = byte;

        /* Escape handling */
        if ( (frame_buffer->escape_buffer & 1u) != 0 )
        {
            /* Restore the bit that had been cleared before transmission */
            BIT_SET(frame_buffer->buffer.data[frame_buffer->buffer.length], FRAME_RESERVED_BIT_POS);
        }
        else
        {
            /* Do nothing - this byte was not escaped */
        }

        /* Ready for next element */
        frame_buffer->escape_buffer = frame_buffer->escape_buffer >> 1;
        frame_buffer->buffer.length++;
    }
}

void _melo_serialize_frame(_m_frame_buffer * const frame_buffer)
{
    uint8_t escape_remaining = 0;
    uint8_t cur_escape_byte  = 0;
    bool    escape_available = false;

    uint8_t data_byte  = 0;
    uint8_t cur_data   = 0;
    uint8_t crc_offset = 0;

    frame_buffer->buffer.length = 0;

    /* HEAD */
    frame_buffer->buffer.data[frame_buffer->buffer.length] = 0x00;
    _melo_create_cmd_byte( &(frame_buffer->buffer.data[frame_buffer->buffer.length]), MELO_CMD_HEAD );
    frame_buffer->buffer.length++;

    /* Length */
    frame_buffer->buffer.data[frame_buffer->buffer.length] = frame_buffer->frame.packet.data.length;
    _melo_create_r( &(frame_buffer->buffer.data[frame_buffer->buffer.length]) );
    frame_buffer->buffer.length++;

    /* Command */
    frame_buffer->buffer.data[frame_buffer->buffer.length] = frame_buffer->frame.packet.command.raw_byte;
    frame_buffer->buffer.length++;

    /* CRC */
    if (frame_buffer->crc_present != false)
    {
        frame_buffer->frame.packet.data.data[frame_buffer->frame.packet.data.length] = 0x66;
        crc_offset = 1u;
    }
    else
    {
        /* Do nothing - no need to use a CRC */
    }

    /* Data */
    for (; cur_data < (frame_buffer->frame.packet.data.length + crc_offset); cur_data++)
    {
        data_byte = frame_buffer->frame.packet.data.data[cur_data];

        if ( IS_FRAME_CONTROL(data_byte) )
        {
            /* Data must be escaped */
            if (escape_available != false)
            {
                /* Existing escape available */
                escape_remaining--;
            }
            else
            {
                /* New escape byte required */
                frame_buffer->buffer.data[frame_buffer->buffer.length] = ESCAPE_BYTE;

                cur_escape_byte  = frame_buffer->buffer.length;
                escape_remaining = NUM_ESCAPE_BYTES - 1u;
                escape_available = true;

                frame_buffer->buffer.length++;
            }

            BIT_SET(frame_buffer->buffer.data[cur_escape_byte], ((NUM_ESCAPE_BYTES - 1u) - escape_remaining) );
            BIT_CLEAR(data_byte, FRAME_RESERVED_BIT_POS);
        }
        else
        {
            if (escape_remaining > 0)
            {
                escape_remaining--;
            }
            else
            {
                /* Do nothing - escape sequence running */
            }
        }

        if ( (escape_available != false) && (escape_remaining == 0) )
        {
            escape_available = false;
            cur_escape_byte  = 0;
        }
        else
        {
            /* Do nothing - escape sequence running */
        }

        frame_buffer->buffer.data[frame_buffer->buffer.length] = data_byte;
        frame_buffer->buffer.length++;
    }

    /* CRC & TAIL */
    frame_buffer->buffer.data[frame_buffer->buffer.length] = 0x00;
    _melo_create_cmd_byte( &(frame_buffer->buffer.data[frame_buffer->buffer.length]), MELO_CMD_TAIL );

    if (frame_buffer->crc_present != false)
    {
        BIT_SET(frame_buffer->buffer.data[0], FRAME_CRC_BIT_POS);
        BIT_SET(frame_buffer->buffer.data[frame_buffer->buffer.length], FRAME_CRC_BIT_POS);
    }
    else
    {
        /* Do nothing - no need to set CRC flags */
    }

    frame_buffer->buffer.length++;
}

static uint8_t _melo_service_handler(const _m_packet * const packet)
{
    uint8_t wait_frame_length;
    bool    success;

    /* Process request */
    send_frame.frame.packet.command.raw_byte = packet->command.raw_byte;

    success = service_table[packet->command.fields.service](packet, &(send_frame.frame.packet));

    if (success != false)
    {
        send_frame.frame.packet.command.fields.status = MELO_CMD_POSITIVE_RESPONSE;
    }
    else
    {
        send_frame.frame.packet.command.fields.status = MELO_CMD_NEGATIVE_RESPONSE;
    }

    /* TODO: send_frame CRC */

    /* Prepare response for Tx */
    _melo_serialize_frame(&send_frame);

    _melo_create_r( &send_frame.buffer.length );
    wait_frame_length = send_frame.buffer.length;

    /* Return size of Tx message */
    return wait_frame_length;
}

static void _melo_packet_handler(const _m_packet * const packet)
{
    if (packet->command.fields.status == MELO_CMD_REQUEST_RESPONSE)
    {
        /* Process incoming request */
        wait_frame.frame.packet.command      = packet->command;
        wait_frame.frame.packet.command.fields.status = MELO_CMD_PENDING_RESPONSE;

        wait_frame.frame.packet.data.length  = 1;
        wait_frame.crc_present               = false;
        wait_frame.frame.packet.data.data[0] = _melo_service_handler(packet);

        _melo_serialize_frame( &wait_frame );
    }
    else if (packet->command.fields.status == MELO_CMD_PENDING_RESPONSE)
    {
        /* Processing pending response */
        MeloRequestBytes( packet->data.data[0] );
    }
    else if (packet->command.fields.status == MELO_CMD_POSITIVE_RESPONSE)
    {
        /* Processing positive response */
        MeloReceiveResponse(packet->command.fields.service, packet->command.fields.subfunction, &(packet->data.data[0]), packet->data.length, true);
    }
    else if (packet->command.fields.status == MELO_CMD_NEGATIVE_RESPONSE)
    {
        /* Processing negative response */
        MeloReceiveResponse(packet->command.fields.service, packet->command.fields.subfunction, &(packet->data.data[0]), packet->data.length, false);
    }
    else
    {
        /* Do nothing - invalid status */
    }
}

static void _melo_frame_handler(const _m_frame * const frame, const bool crc_present)
{
    bool crc_valid = false;

    if (crc_present != false)
    {
        /* Run CRC check */
        if (crc_valid != false)
        {
            _melo_packet_handler( &(frame->packet) );
        }
        else
        {
            /* Error - Invalid CRC */
        }
    }
    else
    {
        _melo_packet_handler( &(frame->packet) );
    }
}

/*[[[cog
import cog
cog.outl("<%include file=\"templates/builtins.tpl\" />")
cog.outl("")
cog.outl("<%include file=\"templates/states.tpl\" />")
]]]*/
/*[[[end]]]*/
