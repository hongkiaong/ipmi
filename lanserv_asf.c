/*
 * lanserv_asf.c
 *
 * MontaVista IPMI RMCP/ASF LAN interface protocol engine
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "lanserv.h"

/* Deal with multi-byte data, RMCP (big-endian) style. */
unsigned int rmcp_get_uint16(unsigned char *data)
{
    return (data[1]
	    | (data[0] << 8));
}

void rmcp_set_uint16(unsigned char *data, int val)
{
    data[1] = val & 0xff;
    data[0] = (val >> 8) & 0xff;
}

unsigned int rmcp_get_uint32(unsigned char *data)
{
    return (data[3]
	    | (data[2] << 8)
	    | (data[1] << 16)
	    | (data[0] << 24));
}

void rmcp_set_uint32(unsigned char *data, int val)
{
    data[3] = val & 0xff;
    data[2] = (val >> 8) & 0xff;
    data[1] = (val >> 16) & 0xff;
    data[0] = (val >> 24) & 0xff;
}

#define ASF_IANA 4542
void
handle_asf(lan_data_t *lan,
	   unsigned char *data, int len,
	   void *from_addr, int from_len)
{
    unsigned char rsp[28];

    if (len < 12)
	return;

    if (rmcp_get_uint32(data+4) != ASF_IANA)
	return; /* Not ASF IANA */

    if (data[8] != 0x80)
	return; /* Not a presence ping. */

    /* Ok, it's a valid RMCP/ASF Presence Ping, start working on the
       response. */
    rsp[0] = 6;
    rsp[1] = 0;
    rsp[2] = 0xff; /* No ack, the ack is not required, so we don't do it. */
    rsp[3] = 6; /* ASF class */
    rmcp_set_uint32(rsp+4, ASF_IANA);
    rsp[8] = 0x40; /* Presense Pong */
    rsp[9] = data[9]; /* Message tag */
    rsp[10] = 0;
    rsp[11] = 16; /* Data length */
    rmcp_set_uint32(rsp+12, ASF_IANA); /* no special capabilities */
    rmcp_set_uint32(rsp+16, 0); /* no special capabilities */
    rsp[20] = 0x81; /* We support IPMI */
    rsp[21] = 0x0; /* No supported interactions */
    memset(rsp+22, 0, 6); /* Reserved. */

    /* Return the response. */
    lan->lan_send(lan->lan_info, data, 28, from_addr, from_len);
}

