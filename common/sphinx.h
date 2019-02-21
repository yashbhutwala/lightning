#ifndef LIGHTNING_COMMON_SPHINX_H
#define LIGHTNING_COMMON_SPHINX_H

#include "config.h"
#include "bitcoin/privkey.h"
#include "bitcoin/pubkey.h"

#include <ccan/short_types/short_types.h>
#include <ccan/tal/tal.h>
#include <secp256k1.h>
#include <sodium/randombytes.h>
#include <wire/gen_onion_wire.h>
#include <wire/wire.h>

#define VERSION_SIZE 1
#define REALM_SIZE 1
#define HMAC_SIZE 32
#define PUBKEY_SIZE 33
#define FRAME_SIZE 65
#define NUM_MAX_FRAMES 20
#define ROUTING_INFO_SIZE (FRAME_SIZE * NUM_MAX_FRAMES)
#define TOTAL_PACKET_SIZE (VERSION_SIZE + PUBKEY_SIZE + HMAC_SIZE + ROUTING_INFO_SIZE)

#if EXPERIMENTAL_FEATURES
#define MAX_FRAMES_PER_HOP (1 << 4)
#else
#define MAX_FRAMES_PER_HOP 1
#endif

struct onionpacket {
	/* Cleartext information */
	u8 version;
	u8 mac[HMAC_SIZE];
	struct pubkey ephemeralkey;

	/* Encrypted information */
	u8 routinginfo[ROUTING_INFO_SIZE];
};

enum route_next_case {
	ONION_END = 0,
	ONION_FORWARD = 1,
};

/**
 * A sphinx payment path.
 *
 * This struct defines a path a payment is taking through the Lightning
 * Network, including the session_key used to generate secrets, the associated
 * data that'll be included in the HMACs and the payloads at each hop in the
 * path. The struct is opaque since it should not be modified externally. Use
 * `sphinx_path_new` or `sphinx_path_new_with_key` (testing only) to create a
 * new instance.
 */
struct sphinx_path;

/* BOLT #4:
 *
 * The `hops_data` field is a structure that holds obfuscations of the
 * next hop's address, transfer information, and its associated HMAC. It is
 * 1300 bytes (`20x65`) long and has the following structure:
 *
 * 1. type: `hops_data`
 * 2. data:
 *    * [`byte`:`realm`]
 *    * [`32*byte`:`per_hop`]
 *    * [`32*byte`:`HMAC`]
 *    * ...
 *    * `filler`
 *
 * Where, the `realm`, `per_hop` (with contents dependent on `realm`), and `HMAC`
 * are repeated for each hop; and where, `filler` consists of obfuscated,
 * deterministically-generated padding, as detailed in
 * [Filler Generation](#filler-generation).  Additionally, `hops_data` is
 * incrementally obfuscated at each hop.
 *
 * The `realm` byte determines the format of the `per_hop` field; currently, only
 * `realm` 0 is defined, for which the `per_hop` format follows:
 *
 * 1. type: `per_hop` (for `realm` 0)
 * 2. data:
 *    * [`short_channel_id`:`short_channel_id`]
 *    * [`u64`:`amt_to_forward`]
 *    * [`u32`:`outgoing_cltv_value`]
 *    * [`12*byte`:`padding`]
 */
struct hop_data {
	u8 realm;
	struct short_channel_id channel_id;
	struct amount_msat amt_forward;
	u32 outgoing_cltv;
};

enum sphinx_payload_type {
	SPHINX_V0_PAYLOAD = 0,
	SPHINX_INVALID_PAYLOAD = 254,
	SPHINX_RAW_PAYLOAD = 255,
};

struct route_step {
	enum route_next_case nextcase;
	struct onionpacket *next;
	u8 realm;
	enum sphinx_payload_type type;
	union {
		struct hop_data v0;
	} payload;
	u8 *raw_payload;
	u8 payload_frames;
};

/**
 * create_onionpacket - Create a new onionpacket that can be routed
 * over a path of intermediate nodes.
 *
 * @ctx: tal context to allocate from
 * @path: public keys of nodes along the path.
 * @hoppayloads: payloads destined for individual hosts (limited to
 *    HOP_PAYLOAD_SIZE bytes)
 * @num_hops: path length in nodes
 * @sessionkey: 32 byte random session key to derive secrets from
 * @assocdata: associated data to commit to in HMACs
 * @assocdatalen: length of the assocdata
 * @path_secrets: (out) shared secrets generated for the entire path
 */
struct onionpacket *create_onionpacket(
	const tal_t * ctx,
	struct sphinx_path *sp,
	struct secret **path_secrets
	);

/**
 * onion_shared_secret - calculate ECDH shared secret between nodes.
 *
 * @secret: the shared secret (32 bytes long)
 * @pubkey: the public key of the other node
 * @privkey: the private key of this node (32 bytes long)
 */
bool onion_shared_secret(
	u8 *secret,
	const struct onionpacket *packet,
	const struct privkey *privkey);

/**
 * process_onionpacket - process an incoming packet by stripping one
 * onion layer and return the packet for the next hop.
 *
 * @ctx: tal context to allocate from
 * @packet: incoming packet being processed
 * @shared_secret: the result of onion_shared_secret.
 * @hoppayload: the per-hop payload destined for the processing node.
 * @assocdata: associated data to commit to in HMACs
 * @assocdatalen: length of the assocdata
 */
struct route_step *process_onionpacket(
	const tal_t * ctx,
	const struct onionpacket *packet,
	const u8 *shared_secret,
	const u8 *assocdata,
	const size_t assocdatalen
	);

/**
 * serialize_onionpacket - Serialize an onionpacket to a buffer.
 *
 * @ctx: tal context to allocate from
 * @packet: the packet to serialize
 */
u8 *serialize_onionpacket(
	const tal_t *ctx,
	const struct onionpacket *packet);

/**
 * parse_onionpacket - Parse an onionpacket from a buffer.
 *
 * @ctx: tal context to allocate from
 * @src: buffer to read the packet from
 * @srclen: length of the @src (must be TOTAL_PACKET_SIZE)
 * @why_bad: if NULL return, this is what was wrong with the packet.
 */
struct onionpacket *parse_onionpacket(const tal_t *ctx,
				      const void *src,
				      const size_t srclen,
				      enum onion_type *why_bad);

struct onionreply {
	/* Node index in the path that is replying */
	int origin_index;
	u8 *msg;
};

/**
 * create_onionreply - Format a failure message so we can return it
 *
 * @ctx: tal context to allocate from
 * @shared_secret: The shared secret used in the forward direction, used for the
 *     HMAC
 * @failure_msg: message (must support tal_len)
 */
u8 *create_onionreply(const tal_t *ctx, const struct secret *shared_secret,
		      const u8 *failure_msg);

/**
 * wrap_onionreply - Add another encryption layer to the reply.
 *
 * @ctx: tal context to allocate from
 * @shared_secret: the shared secret associated with the HTLC, used for the
 *     encryption.
 * @reply: the reply to wrap
 */
u8 *wrap_onionreply(const tal_t *ctx, const struct secret *shared_secret,
		    const u8 *reply);

/**
 * unwrap_onionreply - Remove layers, check integrity and parse reply
 *
 * @ctx: tal context to allocate from
 * @shared_secrets: shared secrets from the forward path
 * @numhops: path length and number of shared_secrets provided
 * @reply: the incoming reply
 */
struct onionreply *unwrap_onionreply(const tal_t *ctx,
				     const struct secret *shared_secrets,
				     const int numhops, const u8 *reply);

/**
 * Create a new empty sphinx_path.
 *
 * The sphinx_path instance can then be decorated with other functions and
 * passed to `create_onionpacket` to generate the packet.
 */
struct sphinx_path *sphinx_path_new(const tal_t *ctx,
				    const u8 *associated_data);

/**
 * Create a new empty sphinx_path with a given `session_key`.
 *
 * This MUST NOT be used outside of tests and tools as it may leak the path
 * details if the `session_key` is not randomly generated.
 */
struct sphinx_path *sphinx_path_new_with_key(const tal_t *ctx,
					     const u8 *associated_data,
					     const struct secret *session_key);

/**
 * Add a V0 (Realm 0) single frame hop to the path.
 */
void sphinx_add_v0_hop(struct sphinx_path *path, const struct pubkey *pubkey,
		       const struct short_channel_id *scid, struct amount_msat forward,
		       u32 outgoing_cltv);
/**
 * Add a raw payload hop to the path.
 */
void sphinx_add_raw_hop(struct sphinx_path *path, const struct pubkey *pubkey,
			u8 realm, const u8 *payload);

#endif /* LIGHTNING_COMMON_SPHINX_H */
