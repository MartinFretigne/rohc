/*
 * Copyright 2010,2011,2012,2013,2014 Didier Barvaux
 * Copyright 2007,2008 Thales Alenia Space
 * Copyright 2007,2009,2010,2012,2013,2014 Viveris Technologies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file c_udp.c
 * @brief ROHC compression context for the UDP profile.
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 * @author Didier Barvaux <didier@barvaux.org>
 */

#include "c_udp.h"
#include "c_ip.h"
#include "rohc_traces_internal.h"
#include "rohc_packets.h"
#include "rohc_utils.h"
#include "crc.h"
#include "rohc_comp_rfc3095.h"
#include "protocols/udp.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>


/**
 * @brief Define the UDP-specific temporary variables in the profile
 *        compression context.
 *
 * This object must be used by the UDP-specific decompression context
 * sc_udp_context.
 *
 * @see sc_udp_context
 */
struct udp_tmp_vars
{
	/** The number of UDP fields that changed in the UDP header */
	int send_udp_dynamic;
};


/**
 * @brief Define the UDP part of the profile decompression context.
 *
 * This object must be used with the generic part of the decompression
 * context rohc_comp_rfc3095_ctxt.
 *
 * @see rohc_comp_rfc3095_ctxt
 */
struct sc_udp_context
{
	/** @brief The number of times the checksum field was added to the
	 *         compressed header */
	size_t udp_checksum_change_count;

	/** The previous UDP header */
	struct udphdr old_udp;

	/** @brief UDP-specific temporary variables that are used during one single
	 *         compression of packet */
	struct udp_tmp_vars tmp;
};


/*
 * Private function prototypes.
 */

static bool c_udp_create(struct rohc_comp_ctxt *const context,
                         const struct rohc_pkt_hdrs *const uncomp_pkt_hdrs)
	__attribute__((warn_unused_result, nonnull(1, 2)));

static rohc_comp_state_t udp_decide_state(const struct rohc_comp_ctxt *const context)
	__attribute__((warn_unused_result, nonnull(1)));

static int c_udp_encode(struct rohc_comp_ctxt *const context,
                        const struct rohc_pkt_hdrs *const uncomp_pkt_hdrs,
                        const struct rohc_ts uncomp_pkt_time,
                        uint8_t *const rohc_pkt,
                        const size_t rohc_pkt_max_len,
                        rohc_packet_t *const packet_type)
	__attribute__((warn_unused_result, nonnull(1, 2, 4, 6)));

static size_t udp_code_dynamic_udp_part(const struct rohc_comp_ctxt *const context,
                                        const uint8_t *const next_header,
                                        uint8_t *const dest,
                                        const size_t counter)
	__attribute__((warn_unused_result, nonnull(1, 2, 3)));

static int udp_changed_udp_dynamic(const struct rohc_comp_ctxt *context,
                                   const struct udphdr *udp);


/**
 * @brief Create a new UDP context and initialize it thanks to the given IP/UDP
 *        packet.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context          The compression context
 * @param uncomp_pkt_hdrs  The uncompressed headers to initialize the new context
 * @return                 true if successful, false otherwise
 */
static bool c_udp_create(struct rohc_comp_ctxt *const context,
                         const struct rohc_pkt_hdrs *const uncomp_pkt_hdrs)
{
	const struct rohc_comp *const comp = context->compressor;
	struct rohc_comp_rfc3095_ctxt *rfc3095_ctxt;
	struct sc_udp_context *udp_context;

	assert(uncomp_pkt_hdrs->innermost_ip_hdr->next_proto == ROHC_IPPROTO_UDP);
	assert(uncomp_pkt_hdrs->udp != NULL);

	/* create and initialize the generic part of the profile context */
	if(!rohc_comp_rfc3095_create(context, uncomp_pkt_hdrs))
	{
		rohc_comp_warn(context, "generic context creation failed");
		goto quit;
	}
	rfc3095_ctxt = (struct rohc_comp_rfc3095_ctxt *) context->specific;

	/* initialize SN to a random value (RFC 3095, 5.11.1) */
	rfc3095_ctxt->sn = comp->random_cb(comp, comp->random_cb_ctxt) & 0xffff;
	rohc_comp_debug(context, "initialize context(SN) = random() = %u",
	                rfc3095_ctxt->sn);

	/* create the UDP part of the profile context */
	udp_context = malloc(sizeof(struct sc_udp_context));
	if(udp_context == NULL)
	{
		rohc_error(context->compressor, ROHC_TRACE_COMP, context->profile->id,
		           "no memory for the UDP part of the profile context");
		goto clean;
	}
	rfc3095_ctxt->specific = udp_context;

	/* initialize the UDP part of the profile context */
	udp_context->udp_checksum_change_count = 0;
	memcpy(&udp_context->old_udp, uncomp_pkt_hdrs->udp, sizeof(struct udphdr));

	/* init the UDP-specific temporary variables */
	udp_context->tmp.send_udp_dynamic = -1;

	/* init the UDP-specific variables and functions */
	rfc3095_ctxt->next_header_len = sizeof(struct udphdr);
	rfc3095_ctxt->decide_state = udp_decide_state;
	rfc3095_ctxt->decide_FO_packet = c_ip_decide_FO_packet;
	rfc3095_ctxt->decide_SO_packet = c_ip_decide_SO_packet;
	rfc3095_ctxt->decide_extension = decide_extension;
	rfc3095_ctxt->init_at_IR = NULL;
	rfc3095_ctxt->get_next_sn = c_ip_get_next_sn;
	rfc3095_ctxt->code_static_part = udp_code_static_udp_part;
	rfc3095_ctxt->code_dynamic_part = udp_code_dynamic_udp_part;
	rfc3095_ctxt->code_ir_remainder = c_ip_code_ir_remainder;
	rfc3095_ctxt->code_UO_packet_head = NULL;
	rfc3095_ctxt->code_uo_remainder = udp_code_uo_remainder;
	rfc3095_ctxt->compute_crc_static = udp_compute_crc_static;
	rfc3095_ctxt->compute_crc_dynamic = udp_compute_crc_dynamic;

	return true;

clean:
	rohc_comp_rfc3095_destroy(context);
quit:
	return false;
}


/**
 * @brief Encode an IP/UDP packet according to a pattern decided by several
 *        different factors.
 *
 * @param context           The compression context
 * @param uncomp_pkt_hdrs   The uncompressed headers to encode
 * @param uncomp_pkt_time   The arrival time of the uncompressed packet
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @param packet_type       OUT: The type of ROHC packet that is created
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int c_udp_encode(struct rohc_comp_ctxt *const context,
                        const struct rohc_pkt_hdrs *const uncomp_pkt_hdrs,
                        const struct rohc_ts uncomp_pkt_time,
                        uint8_t *const rohc_pkt,
                        const size_t rohc_pkt_max_len,
                        rohc_packet_t *const packet_type)
{
	struct rohc_comp_rfc3095_ctxt *const rfc3095_ctxt = context->specific;
	struct sc_udp_context *const udp_context = rfc3095_ctxt->specific;
	int size;

	assert(uncomp_pkt_hdrs->innermost_ip_hdr->next_proto == ROHC_IPPROTO_UDP);
	assert(uncomp_pkt_hdrs->udp != NULL);

	/* how many UDP fields changed? */
	udp_context->tmp.send_udp_dynamic =
		udp_changed_udp_dynamic(context, uncomp_pkt_hdrs->udp);

	/* encode the IP packet */
	size = rohc_comp_rfc3095_encode(context, uncomp_pkt_hdrs, uncomp_pkt_time,
	                                rohc_pkt, rohc_pkt_max_len, packet_type);
	if(size < 0)
	{
		goto quit;
	}

	/* update the context with the new UDP header */
	if((*packet_type) == ROHC_PACKET_IR ||
	   (*packet_type) == ROHC_PACKET_IR_DYN)
	{
		memcpy(&udp_context->old_udp, uncomp_pkt_hdrs->udp, sizeof(struct udphdr));
	}

quit:
	return size;
}


/**
 * @brief Decide the state that should be used for the next packet compressed
 *        with the ROHC UDP profile.
 *
 * The three states are:
 *  - Initialization and Refresh (IR),
 *  - First Order (FO),
 *  - Second Order (SO).
 *
 * @param context  The compression context
 * @return         The new state for the context
 */
static rohc_comp_state_t udp_decide_state(const struct rohc_comp_ctxt *const context)
{
	const struct rohc_comp_rfc3095_ctxt *const rfc3095_ctxt =
		(struct rohc_comp_rfc3095_ctxt *) context->specific;
	const struct sc_udp_context *const udp_context =
		(struct sc_udp_context *) rfc3095_ctxt->specific;
	rohc_comp_state_t next_state;

	if(udp_context->tmp.send_udp_dynamic)
	{
		rohc_comp_debug(context, "go back to IR state because UDP checksum "
		                "behaviour changed in the last few packets");
		next_state = ROHC_COMP_STATE_IR;
	}
	else
	{
		/* generic function used by the IP-only, UDP and UDP-Lite profiles */
		next_state = rohc_comp_rfc3095_decide_state(context);
	}

	return next_state;
}


/**
 * @brief Build UDP-related fields in the tail of the UO packets.
 *
 * \verbatim

     --- --- --- --- --- --- --- ---
    :                               :
 13 +         UDP Checksum          +  2 octets,
    :                               :  if context(UDP Checksum) != 0
     --- --- --- --- --- --- --- ---

\endverbatim
 *
 * @param context     The compression context
 * @param next_header The UDP header
 * @param dest        The rohc-packet-under-build buffer
 * @param counter     The current position in the rohc-packet-under-build buffer
 * @return            The new position in the rohc-packet-under-build buffer
 */
size_t udp_code_uo_remainder(const struct rohc_comp_ctxt *const context,
                             const uint8_t *const next_header,
                             uint8_t *const dest,
                             const size_t counter)
{
	const struct udphdr *const udp = (struct udphdr *) next_header;
	size_t nr_written = 0;

	/* part 13 */
	if(udp->check != 0)
	{
		rohc_comp_debug(context, "UDP checksum = 0x%x", udp->check);
		memcpy(&dest[counter], &udp->check, 2);
		nr_written += 2;
	}

	return counter + nr_written;
}


/**
 * @brief Build the static part of the UDP header.
 *
 * \verbatim

 Static part of UDP header (5.7.7.5):

    +---+---+---+---+---+---+---+---+
 1  /          Source Port          /   2 octets
    +---+---+---+---+---+---+---+---+
 2  /       Destination Port        /   2 octets
    +---+---+---+---+---+---+---+---+

\endverbatim
 *
 * @param context     The compression context
 * @param next_header The UDP header
 * @param dest        The rohc-packet-under-build buffer
 * @param counter     The current position in the rohc-packet-under-build buffer
 * @return            The new position in the rohc-packet-under-build buffer
 */
size_t udp_code_static_udp_part(const struct rohc_comp_ctxt *const context,
                                const uint8_t *const next_header,
                                uint8_t *const dest,
                                const size_t counter)
{
	const struct udphdr *const udp = (struct udphdr *) next_header;
	size_t nr_written = 0;

	/* part 1 */
	rohc_comp_debug(context, "UDP source port = 0x%x", udp->source);
	memcpy(&dest[counter + nr_written], &udp->source, 2);
	nr_written += 2;

	/* part 2 */
	rohc_comp_debug(context, "UDP dest port = 0x%x", udp->dest);
	memcpy(&dest[counter + nr_written], &udp->dest, 2);
	nr_written += 2;

	return counter + nr_written;
}


/**
 * @brief Build the dynamic part of the UDP header.
 *
 * \verbatim

 Dynamic part of UDP header (5.7.7.5):

    +---+---+---+---+---+---+---+---+
 1  /           Checksum            /   2 octets
    +---+---+---+---+---+---+---+---+

\endverbatim
 *
 * @param context     The compression context
 * @param next_header The UDP header
 * @param dest        The rohc-packet-under-build buffer
 * @param counter     The current position in the rohc-packet-under-build buffer
 * @return            The new position in the rohc-packet-under-build buffer
 */
static size_t udp_code_dynamic_udp_part(const struct rohc_comp_ctxt *const context,
                                        const uint8_t *const next_header,
                                        uint8_t *const dest,
                                        const size_t counter)
{
	struct rohc_comp_rfc3095_ctxt *rfc3095_ctxt;
	struct sc_udp_context *udp_context;
	const struct udphdr *udp;
	size_t nr_written = 0;

	rfc3095_ctxt = (struct rohc_comp_rfc3095_ctxt *) context->specific;
	udp_context = (struct sc_udp_context *) rfc3095_ctxt->specific;

	udp = (struct udphdr *) next_header;

	/* part 1 */
	rohc_comp_debug(context, "UDP checksum = 0x%x", udp->check);
	memcpy(&dest[counter + nr_written], &udp->check, 2);
	nr_written += 2;
	udp_context->udp_checksum_change_count++;

	return counter + nr_written;
}


/**
 * @brief Check if the dynamic part of the UDP header changed.
 *
 * @param context The compression context
 * @param udp     The UDP header
 * @return        The number of UDP fields that changed
 */
static int udp_changed_udp_dynamic(const struct rohc_comp_ctxt *context,
                                   const struct udphdr *udp)
{
	const uint8_t oa_repetitions_nr = context->compressor->oa_repetitions_nr;
	const struct rohc_comp_rfc3095_ctxt *rfc3095_ctxt;
	struct sc_udp_context *udp_context;

	rfc3095_ctxt = (struct rohc_comp_rfc3095_ctxt *) context->specific;
	udp_context = (struct sc_udp_context *) rfc3095_ctxt->specific;

	if((udp->check != 0 && udp_context->old_udp.check == 0) ||
	   (udp->check == 0 && udp_context->old_udp.check != 0) ||
	   (udp_context->udp_checksum_change_count < oa_repetitions_nr))
	{
		if((udp->check != 0 && udp_context->old_udp.check == 0) ||
		   (udp->check == 0 && udp_context->old_udp.check != 0))
		{
			udp_context->udp_checksum_change_count = 0;
		}
		return 1;
	}
	else
	{
		return 0;
	}
}


/**
 * @brief Define the compression part of the UDP profile as described
 *        in the RFC 3095.
 */
const struct rohc_comp_profile c_udp_profile =
{
	.id             = ROHC_PROFILE_UDP, /* profile ID (see 8 in RFC 3095) */
	.create         = c_udp_create,     /* profile handlers */
	.destroy        = rohc_comp_rfc3095_destroy,
	.encode         = c_udp_encode,
	.feedback       = rohc_comp_rfc3095_feedback,
};

