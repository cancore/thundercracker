/*
 * Thundercracker Firmware -- Confidential, not for redistribution.
 * Copyright <c> 2012 Sifteo, Inc. All rights reserved.
 */

#include <protocol.h>
#include "macros.h"
#include "cubeconnector.h"
#include "cube.h"
#include "cubeslots.h"
#include "neighbor_tx.h"
#include "neighbor_protocol.h"
#include "systime.h"
#include "radioaddrfactory.h"
#include "prng.h"
#include "tasks.h"

#ifdef SIFTEO_SIMULATOR
#   include "system.h"
#   include "system_mc.h"
#endif

RadioAddress CubeConnector::pairingAddr = { 0, RF_PAIRING_ADDRESS };
RadioAddress CubeConnector::connectionAddr;
RadioAddress CubeConnector::reconnectAddr;

_SYSPseudoRandomState CubeConnector::prng;
RingBuffer<RadioManager::FIFO_DEPTH, uint8_t, uint8_t> CubeConnector::rxState;

SysLFS::PairingIDRecord CubeConnector::savedPairingID;
SysLFS::PairingMRURecord CubeConnector::savedPairingMRU;
BitVector<SysLFS::NUM_PAIRINGS> CubeConnector::reconnectQueue;
BitVector<SysLFS::NUM_PAIRINGS> CubeConnector::recycleQueue;
BitVector<CubeConnector::NUM_WORK_ITEMS> CubeConnector::taskWork;

uint8_t CubeConnector::neighborKey;
uint8_t CubeConnector::txState;
uint8_t CubeConnector::pairingPacketCounter;
uint8_t CubeConnector::hwid[HWID_LEN];
_SYSCubeID CubeConnector::cubeID;
SysLFS::Key CubeConnector::cubeRecord;


void CubeConnector::init()
{
    // Seed our PRNG with an unguessable number, unique to this system
    Crc32::reset();
    Crc32::addUniqueness();
    PRNG::init(&prng, Crc32::get());

    // Set a default master ID
    neighborKey = ~0;
    nextNeighborKey();
    ASSERT(neighborKey < Neighbor::NUM_MASTER_ID);

    // Load saved pairing HWIDs from SysLFS
    savedPairingID.load();
    savedPairingMRU.load();

    // State machine init
    txState = PairingFirstContact;
    rxState.init();
}

void CubeConnector::task()
{
    /*
     * Handle work items from our taskWork vector. New work items may be
     * set pending at any time by the ISR, so we need to be sure to
     * always clear flags before performing the associated work, and we
     * need to use atomic ops.
     */

    BitVector<NUM_WORK_ITEMS> pending = taskWork;
    unsigned index;
    while (pending.clearFirst(index)) {
        taskWork.atomicClear(index);
        switch (index) {
 
            case TaskSavePairingID:
                SysLFS::write(SysLFS::kPairingID, savedPairingID);
                break;

            case TaskSavePairingMRU:
                SysLFS::write(SysLFS::kPairingMRU, savedPairingMRU);
                break;

            case TaskRecyclePairings:
                while (recycleQueue.findFirst(index)) {
                    recycleQueue.atomicClear(index);
                    SysLFS::deleteCube(index);
                }
                break;

        }
    }
}

void CubeConnector::setNeighborKey(unsigned k)
{
    /*
     * Set the random 3-bit key which determines our neighbor ID and pairing channel.
     */

    ASSERT(k < Neighbor::NUM_MASTER_ID);
    neighborKey = k;

    unsigned idByte = Neighbor::FIRST_MASTER_ID + k;
    NeighborTX::start((idByte << 8) | uint8_t(~idByte << 3), ~0);

    static const uint8_t channels[] = RF_PAIRING_CHANNELS;
    pairingAddr.channel = channels[k];
}

void CubeConnector::nextNeighborKey()
{
    /*
     * Choose a random neighbor key, using any value other than the current key.
     * Use entropy from the current time, the system-unique ID, and all previous calls
     * to this same function.
     *
     * We're careful that this value is hard to guess as well as uniformly distributed,
     * so we make the best use of our very limited ID space.
     */

    PRNG::collectTimingEntropy(&prng);

    unsigned newKey;
    if (neighborKey < Neighbor::NUM_MASTER_ID) {
        // Replacing a valid key. Avoid picking the same one.
        newKey = PRNG::valueBounded(&prng, Neighbor::NUM_MASTER_ID - 2);
        if (newKey >= neighborKey)
            newKey++;
    } else {
        // Not replacing any key. Choose any one.
        newKey = PRNG::valueBounded(&prng, Neighbor::NUM_MASTER_ID - 1);
    }

    setNeighborKey(newKey);
}

bool CubeConnector::chooseConnectionAddr()
{
    /*
     * Pick a new random connection channel and address, for use by
     * a new cube that we want to connect. These numbers need to be
     * random and difficult to guess, but we don't require a secure RNG
     * since obviously we're still transmitting in cleartext :)
     *
     * Channel can be any value within range.
     *
     * Addresses should avoid bytes 0x00, 0x55, 0xAA, and 0xFF.
     * Explicitly exclude these from our random number space, while
     * otherwise keeping it perfectly uniform.
     *
     * If no more cube IDs are available, we return false.
     * Otherwise, we return true after successfully initializing the
     * connection channel, address, and cube ID.
     */

    PRNG::collectTimingEntropy(&prng);
    RadioAddrFactory::random(connectionAddr, prng);

    // Pick a cube ID, based on what's available right now.
    _SYSCubeIDVector cv = CubeSlots::availableSlots();
    if (cv) {
        cubeID = Intrinsic::CLZ(cv);
        return true;
    } else {
        return false;
    }
}

void CubeConnector::produceRadioHop(PacketBuffer &buf)
{
    /*
     * Assemble a packet containing only a Radio Hop to the current
     * connectionAddr and cubeID.
     */

    buf.len = 8;
    buf.bytes[0] = 0x7a;
    buf.bytes[1] = connectionAddr.channel;
    memcpy(&buf.bytes[2], connectionAddr.id, 5);
    buf.bytes[7] = 0xE0 | cubeID;
}

void CubeConnector::refillReconnectQueue()
{
    /*
     * This is how we establish our round-robin schedule for sharing bandwidth
     * between pairing and reconnection attempts. This queue contains all pairing
     * IDs that we still need to transmit to. When this queue runs out, we try to
     * pair a new cube before refilling the round-robin queue.
     *
     * This queue includes all pairing slots which aren't associated with
     * currently-connected cubes, and which have plausibly valid HWIDs stored.
     */

    for (unsigned i = 0; i < SysLFS::NUM_PAIRINGS; ++i) {

        if (CubeSlots::pairConnected.test(i))
            continue;

        if (savedPairingID.hwid[i] == SysLFS::PairingIDRecord::INVALID_HWID)
            continue;

        reconnectQueue.mark(i);
    }
}

bool CubeConnector::popReconnectQueue()
{
    /*
     * Extract the next reconnectable cube from our queue, and use it to
     * set the current hwid and reconnectAddr.
     */

    // Reconnection can be disabled in Siftulator. This is used by cube firmware unit tests.
    #ifdef SIFTEO_SIMULATOR
    if (SystemMC::getSystem()->opt_noCubeReconnect)
        return false;
    #endif

    unsigned index;
    if (!reconnectQueue.clearFirst(index))
        return false;

    uint64_t hwid64 = savedPairingID.hwid[index];
    memcpy(hwid, &hwid64, sizeof hwid);
    RadioAddrFactory::fromHardwareID(reconnectAddr, hwid64);
    cubeRecord = SysLFS::Key(SysLFS::kCubeBase + index);

    return true;
}

void CubeConnector::newCubeRecord()
{
    /*
     * Set cubeRecord based on the least recently used ID
     * which isn't currently connected.
     *
     * Recycle the existing slot, and write our new pairing
     * data there. In the event every slot is full, we set
     * cubeRecord arbitrarily without udpating hwid[],
     * so that the pairing will fail.
     */

    unsigned index;
    for (int i = SysLFS::NUM_PAIRINGS - 1; i >= 0; --i) {
        index = savedPairingMRU.rank[i];
        if (!CubeSlots::pairConnected.test(index)) {

            // Remember to delete old SysLFS information about this cube
            recycleQueue.atomicMark(index);
            taskWork.atomicMark(TaskRecyclePairings);
            Tasks::trigger(Tasks::CubeConnector);

            // Write the new HWID
            memcpy(&savedPairingID.hwid[index], hwid, HWID_LEN);
            taskWork.atomicMark(TaskSavePairingID);
            Tasks::trigger(Tasks::CubeConnector);

            break;
        }
    }

    cubeRecord = SysLFS::Key(SysLFS::kCubeBase + index);
}

void CubeConnector::radioProduce(PacketTransmission &tx)
{
    switch (txState) {

        /*
         * Our first chance to talk to a new cube who's in range of our
         * neighbor transmitters. If it detects the beacon we're sending
         * out with our current neighbor key, it will be listening on the
         * corresponding channel.
         *
         * We just send a mininal ping packet, since we (a) don't want to
         * waste the time sending a longer packet that will most likely be
         * ignored, and (b) we don't want to hop until we've confirmed that
         * we can talk to the cube and that it's physically neighbored.
         *
         * We periodically hop to the next neighbor key, not because we need
         * to for verification purposes, but to help us avoid spamming any
         * single radio frequency too badly. We do this based on the number
         * of packets sent since the last key hop, so the hop rate scales
         * with our transmit rate.
         *
         * Right now we just hop every time an 8-bit packet counter overflows.
         * At the fastest, this equates to about one hop per second. Note that
         * this is designed so that we also call nextNeighborKey() on the
         * very first radioProduce() after the counter is initialized to zero.
         */
        case PairingFirstContact:
        case_PairingFirstContact:
            refillReconnectQueue();
            if (!(pairingPacketCounter++)) {
                nextNeighborKey();
            }
            tx.dest = &pairingAddr;
            tx.packet.len = 1;
            tx.packet.bytes[0] = 0xff;
            tx.numSoftwareRetries = 0;
            tx.numHardwareRetries = 0;
            rxState.enqueue(PairingFirstContact);
            break;

        /*
         * Similarly, this is our first chance to talk to a paired cube
         * that we aren't yet connected with. This same radio packet has
         * the dual purpose of alerting us to the presence of such a
         * cube, and waking that cube up from sleep.
         *
         * We toggle between a primary and an alternate channel for each
         * reconnectable address.
         */
        case ReconnectFirstContact:
        case_ReconnectFirstContact:
            if (popReconnectQueue()) {
                tx.dest = &reconnectAddr;
                tx.packet.len = 1;
                tx.packet.bytes[0] = 0xff;
                tx.numSoftwareRetries = 0;
                tx.numHardwareRetries = 0;
                rxState.enqueue(ReconnectFirstContact);
                break;
            }
            goto case_PairingFirstContact;

        case ReconnectAltFirstContact:
            RadioAddrFactory::channelToggle(reconnectAddr);
            tx.dest = &reconnectAddr;
            tx.packet.len = 1;
            tx.packet.bytes[0] = 0xff;
            tx.numSoftwareRetries = 0;
            tx.numHardwareRetries = 0;
            rxState.enqueue(txState);
            break;

        /*
         * After establishing first contact, we gain some trust that we're
         * talking to a physically-neighbored cube by performing a sequence
         * of channel hops and waiting for the cube to follow. This would
         * be easy to spoof, but the chances of randomly "stealing" another
         * base's pairing event becomes extremely low.
         *
         * The ping packet we transmit is identical to first contact, except
         * we do want a small amount of retry. Not too much, or we open the
         * window for false positives. Currently the defaults seem fine.
         */
        case PairingFirstVerify ... PairingFinalVerify:
            tx.dest = &pairingAddr;
            tx.packet.len = 1;
            tx.packet.bytes[0] = 0xff;
            rxState.enqueue(txState);
            break;

        /*
         * Once our pairing verification is done, send the cube a hop
         * to connect it on an address and channel of our choice.
         */
        case PairingBeginHop:
            newCubeRecord();
            if (chooseConnectionAddr()) {
                tx.dest = &pairingAddr;
                produceRadioHop(tx.packet);
                rxState.enqueue(PairingBeginHop);
                break;
            }
            goto case_PairingFirstContact;

        /*
         * Similarly, begin a hop after reconnection
         */
        case ReconnectBeginHop:
            if (chooseConnectionAddr()) {
                tx.dest = &reconnectAddr;
                produceRadioHop(tx.packet);
                rxState.enqueue(ReconnectBeginHop);
                break;
            }
            goto case_ReconnectFirstContact;

        /*
         * We think we just hopped to a new connection-specific address and
         * channel. See if we can regain contact with the cube at this new
         * address, to verify that the hop worked.
         *
         * Since we don't get a full ACK automatically any more due to
         * having been disconnected, we'll need to explicitly request one
         * with the Explicit Full ACK code.
         */
        case HopConfirm:
            tx.dest = &connectionAddr;
            tx.packet.len = 1;
            tx.packet.bytes[0] = 0x79;
            rxState.enqueue(txState);
            break;
    };
}

void CubeConnector::radioAcknowledge(const PacketBuffer &packet)
{
    RF_ACKType *ack = (RF_ACKType *) packet.bytes;
    unsigned packetRxState = rxState.dequeue();
    switch (packetRxState) {

        /*
         * When we get a response to the first packet, start
         * a sequence of channel hops which will allow us to verify that
         * this cube is neighbored with us and not a different base.
         *
         * Store the HWID, so we can check it during each verify.
         */
        case PairingFirstContact:
            nextNeighborKey();
            if (packet.len >= RF_ACK_LEN_HWID) {
                memcpy(hwid, ack->hwid, sizeof hwid);
                txState = packetRxState + 1;
            }
            break;

        /*
         * For each verification stage, check the HWID and proceed to the
         * next stage if everything looks okay.
         */
        case PairingFirstVerify ... PairingFinalVerify:
            nextNeighborKey();
            if (packet.len >= RF_ACK_LEN_HWID && !memcmp(hwid, ack->hwid, sizeof hwid)) {
                txState = packetRxState + 1;
            } else {
                txState = PairingFirstContact;
            }
            break;

        /*
         * We got a response back from a paired cube that we'd like to connect!
         * Assuming the HWID matches what we have on file, continue the
         * connection attempt by beginning a hop.
         */
        case ReconnectFirstContact:
        case ReconnectAltFirstContact:
            if (packet.len >= RF_ACK_LEN_HWID && !memcmp(hwid, ack->hwid, sizeof hwid)) {
                txState = ReconnectBeginHop;
            }
            break;

        /*
         * We got a successful response back from the hop packet. Okay,
         * but we'll still want to verify that it actaully shows up on
         * the new address too.
         */
        case PairingBeginHop:
        case ReconnectBeginHop:
            txState = HopConfirm;
            break;

        /*
         * Woo, hop confirmed. Assuming the HWID matches and the cubeID
         * we chose is still available, this means the new cube has
         * finished connecting and we can hand it off to a CubeSlot.
         */
        case HopConfirm:
            if (packet.len >= RF_ACK_LEN_HWID && !memcmp(hwid, ack->hwid, sizeof hwid)) {

                // Mark this pairing as "recently used", so we don't recycle it
                if (savedPairingMRU.access(cubeRecord - SysLFS::kCubeBase)) {
                    taskWork.atomicMark(TaskSavePairingMRU);
                    Tasks::trigger(Tasks::CubeConnector);
                }

                // Connect the cube!
                CubeSlot &cube = CubeSlots::instances[cubeID];
                if (cube.isSlotAvailable()) {
                    cube.connect(cubeRecord, connectionAddr, *ack);
                }
            }
            txState = PairingFirstContact;
            break;
    }
}

void CubeConnector::radioTimeout()
{
    unsigned packetRxState = rxState.dequeue();
    switch (packetRxState) {

        /*
         * Our hop packet timed out. That's actually fine, since it
         * may just mean that the cube hopped successfully but we
         * lost the ACK packet. Go ahead and try to check for it on
         * the new address.
         */
        case PairingBeginHop:
        case ReconnectBeginHop:
            txState = HopConfirm;
            break;

        /*
         * If we timed out while trying to reconnect, hop over to the
         * alternate channel before giving up totally.
         */
        case ReconnectFirstContact:
            txState = ReconnectAltFirstContact;
            break;

        /*
         * In all other cases, abort what we were doing and send out
         * a new spam packet to someone. Who? That's determined by
         * our reconnectQueue.
         *
         * If there's a reconnectable HWID in the queue, we head to
         * ReconnectFirstContact to try poking it. If not, that will
         * fall through to PairingFirstContact and refill the queue.
         */
        default:
            txState = ReconnectFirstContact;
            break;
    }
}

void CubeConnector::radioEmptyAcknowledge()
{
    /*
     * Empty ACKs don't really mean anything to us, since a disconnected
     * cube should always be sending us a full ACK packet (which we need
     * in order to verify its identity). So, just dequeue the state and
     * don't act on it.
     */
    rxState.dequeue();
}