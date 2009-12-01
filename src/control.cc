/* $Id$
 *
 * Network Performance Meter
 * Copyright (C) 2009 by Thomas Dreibholz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Contact: dreibh@iem.uni-due.de
 */

#include "control.h"
#include "tools.h"

#include <string.h>
#include <math.h>
#include <poll.h>
#include <assert.h>

#include <iostream>


unsigned int         gOutputVerbosity = 9;
extern MessageReader gMessageReader;


// ##########################################################################
// #### Active Side Control                                              ####
// ##########################################################################


#define IDENTIFY_MAX_TRIALS 10
#define IDENTIFY_TIMEOUT    30000


// ###### Download file #####################################################
static bool downloadOutputFile(MessageReader* messageReader,
                               const int      controlSocket,
                               const char*    fileName)
{
   char                 messageBuffer[sizeof(NetPerfMeterResults) +
                                      NETPERFMETER_RESULTS_MAX_DATA_LENGTH];
   NetPerfMeterResults* resultsMsg = (NetPerfMeterResults*)&messageBuffer;

   FILE* fh = fopen(fileName, "w");
   if(fh == NULL) {
      std::cerr << "ERROR: Unable to create file " << fileName
               << " - " << strerror(errno) << "!" << std::endl;
      return(false);
   }
   bool success = false;
   ssize_t received = gMessageReader.receiveMessage(controlSocket, resultsMsg, sizeof(messageBuffer));
   while( (received == MRRM_PARTIAL_READ) || (received >= (ssize_t)sizeof(NetPerfMeterResults)) ) {
      if(received > 0) {
         const size_t bytes = ntohs(resultsMsg->Header.Length);
         if(resultsMsg->Header.Type != NETPERFMETER_RESULTS) {
            std::cerr << "ERROR: Received unexpected message type "
                      << (unsigned int)resultsMsg->Header.Type <<  "!" << std::endl;
            exit(1);
         }
         if(bytes != (size_t)received) {
            std::cerr << "ERROR: Received malformed NETPERFMETER_RESULTS message!" << std::endl;
            printf("%u + %u > %u\n", (unsigned int)bytes, (unsigned int)sizeof(NetPerfMeterResults), (unsigned int)received);
            exit(1);
         }
         if(gOutputVerbosity >= NPFOV_REALLY_VERBOSE) {
            std::cout << ".";
         }

         if(bytes > 0) {
            if(fwrite((char*)&resultsMsg->Data, bytes, 1, fh) != 1) {
               std::cerr << "ERROR: Unable to write results to file " << fileName
                         << " - " << strerror(errno) << "!" << std::endl;
               exit(1);
            }
         }
         else {
            std::cerr << "WARNING: No results received for "
                      << fileName << "!" << std::endl;
         }

         if(resultsMsg->Header.Flags & NPMRF_EOF) {
            if(gOutputVerbosity >= NPFOV_STATUS) {
               std::cout << " <done>"; std::cout.flush();
            }
            success = true;
            break;
         }
      }
      received = gMessageReader.receiveMessage(controlSocket, resultsMsg, sizeof(messageBuffer));
   }
   if(!success) {
      std::cerr << std::endl
                << "ERROR: Unable to download results (errno=" << errno << "): "
                << strerror(errno) << std::endl;
   }
   fclose(fh);
   return(success);
}


// ###### Download statistics file ##########################################
static bool downloadResults(MessageReader*     messageReader,
                            const int          controlSocket,
                            const std::string& namePattern,
                            const Flow*        flow)
{
   const std::string outputName =
      Flow::getNodeOutputName(
         namePattern, "passive",
         format("-%08x-%04x", flow->getFlowID(), flow->getStreamID()));

   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << "Downloading results [" << outputName << "] ";
      std::cout.flush();
   }
   bool success = awaitNetPerfMeterAcknowledge(messageReader, controlSocket,
                                               flow->getMeasurementID(),
                                               flow->getFlowID(),
                                               flow->getStreamID());
   if(success) {
      if(gOutputVerbosity >= NPFOV_STATUS) {
         std::cout << "<ack> "; std::cout.flush();
      }
      success = downloadOutputFile(messageReader, controlSocket, outputName.c_str());
   }

   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << " " << ((success == true) ? "okay": "FAILED") << std::endl;
   }
   return(success);
}


// ###### Tell remote node to add new flow ##################################
bool performNetPerfMeterAddFlow(MessageReader* messageReader,
                                int            controlSocket,
                                const Flow*    flow)
{
   // ====== Sent NETPERFMETER_ADD_FLOW to remote node ======================
   const size_t                addFlowMsgSize = sizeof(NetPerfMeterAddFlowMessage) +
                                                   (sizeof(uint32_t) * flow->getTrafficSpec().OnOffEvents.size());
   char                        addFlowMsgBuffer[addFlowMsgSize];
   NetPerfMeterAddFlowMessage* addFlowMsg = (NetPerfMeterAddFlowMessage*)&addFlowMsgBuffer;
   addFlowMsg->Header.Type   = NETPERFMETER_ADD_FLOW;
   addFlowMsg->Header.Flags  = 0x00;
   addFlowMsg->Header.Length = htons(addFlowMsgSize);
   addFlowMsg->MeasurementID = hton64(flow->getMeasurementID());
   addFlowMsg->FlowID        = htonl(flow->getFlowID());
   addFlowMsg->StreamID      = htons(flow->getStreamID());
   addFlowMsg->Protocol      = flow->getTrafficSpec().Protocol;
   addFlowMsg->Flags         = 0x00;
   for(size_t i = 0;i < NETPERFMETER_RNG_INPUT_PARAMETERS;i++) {
      addFlowMsg->FrameRate[i] = doubleToNetwork(flow->getTrafficSpec().InboundFrameRate[i]);
      addFlowMsg->FrameSize[i] = doubleToNetwork(flow->getTrafficSpec().InboundFrameSize[i]);
   }
   addFlowMsg->FrameRateRng  = flow->getTrafficSpec().InboundFrameRateRng;
   addFlowMsg->FrameSizeRng  = flow->getTrafficSpec().InboundFrameSizeRng;
   addFlowMsg->RcvBufferSize = htonl(flow->getTrafficSpec().RcvBufferSize);
   addFlowMsg->SndBufferSize = htonl(flow->getTrafficSpec().SndBufferSize);
   addFlowMsg->MaxMsgSize    = htons(flow->getTrafficSpec().MaxMsgSize);
   addFlowMsg->OrderedMode   = htonl((uint32_t)((long long)rint(flow->getTrafficSpec().OrderedMode * (double)0xffffffff)));
   addFlowMsg->ReliableMode  = htonl((uint32_t)((long long)rint(flow->getTrafficSpec().ReliableMode * (double)0xffffffff)));
   addFlowMsg->RetransmissionTrials =
      htonl((uint32_t)flow->getTrafficSpec().RetransmissionTrials |
            (uint32_t)(flow->getTrafficSpec().RetransmissionTrialsInMS ? NPMAF_RTX_TRIALS_IN_MILLISECONDS : 0));

   addFlowMsg->Padding     = 0x0000;
   addFlowMsg->OnOffEvents = htons(flow->getTrafficSpec().OnOffEvents.size());
   size_t i = 0;
   for(std::set<unsigned int>::iterator iterator = flow->getTrafficSpec().OnOffEvents.begin();
       iterator != flow->getTrafficSpec().OnOffEvents.end();iterator++, i++) {
      addFlowMsg->OnOffEvent[i] = htonl(*iterator);
   }
   memset((char*)&addFlowMsg->Description, 0, sizeof(addFlowMsg->Description));
   strncpy((char*)&addFlowMsg->Description, flow->getTrafficSpec().Description.c_str(),
           std::min(sizeof(addFlowMsg->Description), flow->getTrafficSpec().Description.size()));

   sctp_sndrcvinfo sinfo;
   memset(&sinfo, 0, sizeof(sinfo));
   sinfo.sinfo_ppid = htonl(PPID_NETPERFMETER_CONTROL);

   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << "<R1> "; std::cout.flush();
   }
   if(sctp_send(controlSocket, addFlowMsg, addFlowMsgSize, &sinfo, 0) <= 0) {
      return(false);
   }

   // ====== Wait for NETPERFMETER_ACKNOWLEDGE ==============================
   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << "<R2> "; std::cout.flush();
   }
   if(awaitNetPerfMeterAcknowledge(messageReader, controlSocket,
                                   flow->getMeasurementID(),
                                   flow->getFlowID(), flow->getStreamID()) == false) {
      return(false);
   }

   // ======  Let remote identify the new flow ==============================
   return(performNetPerfMeterIdentifyFlow(messageReader, controlSocket, flow));
}


// ###### Let remote identify a new flow ####################################
bool performNetPerfMeterIdentifyFlow(MessageReader* messageReader,
                                     int            controlSocket,
                                     const Flow*    flow)
{
   // ====== Sent NETPERFMETER_IDENTIFY_FLOW to remote node =================
   unsigned int maxTrials = 1;
   if( (flow->getTrafficSpec().Protocol != IPPROTO_SCTP) &&
       (flow->getTrafficSpec().Protocol != IPPROTO_TCP) ) {
      // SCTP and TCP are reliable transport protocols => no retransmissions
      maxTrials = IDENTIFY_MAX_TRIALS;
   }
   for(unsigned int trial = 0;trial < maxTrials;trial++) {
      NetPerfMeterIdentifyMessage identifyMsg;
      identifyMsg.Header.Type   = NETPERFMETER_IDENTIFY_FLOW;
      identifyMsg.Header.Flags  = hasSuffix(flow->getVectorFile().getName(), ".bz2") ? NPMIF_COMPRESS_VECTORS : 0x00;
      identifyMsg.Header.Length = htons(sizeof(identifyMsg));
      identifyMsg.MagicNumber   = hton64(NETPERFMETER_IDENTIFY_FLOW_MAGIC_NUMBER);
      identifyMsg.MeasurementID = hton64(flow->getMeasurementID());
      identifyMsg.FlowID        = htonl(flow->getFlowID());
      identifyMsg.StreamID      = htons(flow->getStreamID());

      if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
         std::cout << "<R3> "; std::cout.flush();
      }
      if(flow->getTrafficSpec().Protocol == IPPROTO_SCTP) {
         sctp_sndrcvinfo sinfo;
         memset(&sinfo, 0, sizeof(sinfo));
         sinfo.sinfo_stream = flow->getStreamID();
         sinfo.sinfo_ppid   = htonl(PPID_NETPERFMETER_CONTROL);
         if(sctp_send(flow->getSocketDescriptor(), &identifyMsg, sizeof(identifyMsg), &sinfo, 0) <= 0) {
            return(false);
         }
      }
      else {
         if(ext_send(flow->getSocketDescriptor(), &identifyMsg, sizeof(identifyMsg), 0) <= 0) {
            return(false);
         }
      }
      if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
         std::cout << "<R4> "; std::cout.flush();
      }
      if(awaitNetPerfMeterAcknowledge(messageReader, controlSocket,
                                      flow->getMeasurementID(),
                                      flow->getFlowID(), flow->getStreamID(),
                                      IDENTIFY_TIMEOUT) == true) {
         return(true);
      }
   }
   return(false);
}


// ###### Start measurement #################################################
bool performNetPerfMeterStart(MessageReader*         messageReader,
                              int                    controlSocket,
                              const uint64_t         measurementID,
                              const char*            activeNodeName,
                              const char*            passiveNodeName,
                              const char*            configName,
                              const char*            vectorNamePattern,
                              const OutputFileFormat vectorFileFormat,
                              const char*            scalarNamePattern,
                              const OutputFileFormat scalarFileFormat)
{
   // ====== Write config file ==============================================
   FILE* configFile = NULL;
   if(configName[0] != 0x00) {
      configFile = fopen(configName, "w");
      if(configFile == NULL) {
         std::cerr << "ERROR: Unable to create config file <"
                  << configName << ">!" << std::endl;
         return(false);
      }

      FlowManager::getFlowManager()->lock();
      
      fprintf(configFile, "NUM_FLOWS=%u\n", (unsigned int)FlowManager::getFlowManager()->getFlowSet().size());
      fprintf(configFile, "NAME_ACTIVE_NODE=\"%s\"\n", activeNodeName);
      fprintf(configFile, "NAME_PASSIVE_NODE=\"%s\"\n", passiveNodeName);
      fprintf(configFile, "SCALAR_ACTIVE_NODE=\"%s\"\n",
                           Flow::getNodeOutputName(scalarNamePattern, "active").c_str());
      fprintf(configFile, "SCALAR_PASSIVE_NODE=\"%s\"\n",
                           Flow::getNodeOutputName(scalarNamePattern, "passive").c_str());
      fprintf(configFile, "VECTOR_ACTIVE_NODE=\"%s\"\n",
                           Flow::getNodeOutputName(vectorNamePattern, "active").c_str());
      fprintf(configFile, "VECTOR_PASSIVE_NODE=\"%s\"\n\n",
                           Flow::getNodeOutputName(vectorNamePattern, "passive").c_str());
   
      for(std::vector<Flow*>::iterator iterator = FlowManager::getFlowManager()->getFlowSet().begin();
         iterator != FlowManager::getFlowManager()->getFlowSet().end();
         iterator++) {
         const Flow* flow = *iterator;
         char extension[32];
         snprintf((char*)&extension, sizeof(extension), "-%08x-%04x",        flow->getFlowID(), flow->getStreamID());
         fprintf(configFile, "FLOW%u_DESCRIPTION=\"%s\"\n",                  flow->getFlowID(), flow->getTrafficSpec().Description.c_str());
         fprintf(configFile, "FLOW%u_PROTOCOL=\"%s\"\n",                     flow->getFlowID(), getProtocolName(flow->getTrafficSpec().Protocol));
         for(unsigned int i = 0;i < NETPERFMETER_RNG_INPUT_PARAMETERS;i++) {
            fprintf(configFile, "FLOW%u_OUTBOUND_FRAME_RATE_VALUE%u=%f\n",   flow->getFlowID(), i + 1, flow->getTrafficSpec().OutboundFrameRate[i]);
         }
         fprintf(configFile, "FLOW%u_OUTBOUND_FRAME_RATE_RNG_ID=%u\n",       flow->getFlowID(), flow->getTrafficSpec().OutboundFrameRateRng);
         fprintf(configFile, "FLOW%u_OUTBOUND_FRAME_RATE_RNG_NAME=\"%s\"\n", flow->getFlowID(), getRandomGeneratorName(flow->getTrafficSpec().OutboundFrameRateRng));
         for(unsigned int i = 0;i < NETPERFMETER_RNG_INPUT_PARAMETERS;i++) {
            fprintf(configFile, "FLOW%u_OUTBOUND_FRAME_SIZE_VALUE%u=%f\n",   flow->getFlowID(), i + 1, flow->getTrafficSpec().OutboundFrameSize[i]);
         }
         fprintf(configFile, "FLOW%u_OUTBOUND_FRAME_SIZE_RNG=%u\n",          flow->getFlowID(), flow->getTrafficSpec().OutboundFrameSizeRng);
         for(unsigned int i = 0;i < NETPERFMETER_RNG_INPUT_PARAMETERS;i++) {
            fprintf(configFile, "FLOW%u_INBOUND_FRAME_RATE_VALUE%u=%f\n",   flow->getFlowID(), i + 1, flow->getTrafficSpec().InboundFrameRate[i]);
         }
         fprintf(configFile, "FLOW%u_INBOUND_FRAME_RATE_RNG_ID=%u\n",        flow->getFlowID(), flow->getTrafficSpec().InboundFrameRateRng);
         fprintf(configFile, "FLOW%u_INBOUND_FRAME_RATE_RNG_NAME=\"%s\"\n",  flow->getFlowID(), getRandomGeneratorName(flow->getTrafficSpec().InboundFrameRateRng));
         for(unsigned int i = 0;i < NETPERFMETER_RNG_INPUT_PARAMETERS;i++) {
            fprintf(configFile, "FLOW%u_INBOUND_FRAME_SIZE_VALUE%u=%f\n",   flow->getFlowID(), i + 1, flow->getTrafficSpec().InboundFrameSize[i]);
         }
         fprintf(configFile, "FLOW%u_INBOUND_FRAME_SIZE_RNG=%u\n",           flow->getFlowID(), flow->getTrafficSpec().InboundFrameSizeRng);
         fprintf(configFile, "FLOW%u_RELIABLE=%f\n",                         flow->getFlowID(), flow->getTrafficSpec().ReliableMode);
         fprintf(configFile, "FLOW%u_ORDERED=%f\n",                          flow->getFlowID(), flow->getTrafficSpec().OrderedMode);
         fprintf(configFile, "FLOW%u_VECTOR_ACTIVE_NODE=\"%s\"\n",           flow->getFlowID(), flow->getVectorFile().getName().c_str());
         fprintf(configFile, "FLOW%u_VECTOR_PASSIVE_NODE=\"%s\"\n\n",        flow->getFlowID(),
                              Flow::getNodeOutputName(vectorNamePattern, "passive",
                                 format("-%08x-%04x", flow->getFlowID(), flow->getStreamID())).c_str());
      }         

      FlowManager::getFlowManager()->unlock();
      
      fclose(configFile);
   }

   // ====== Start flows ====================================================
   const bool success = FlowManager::getFlowManager()->startMeasurement(
                           measurementID, getMicroTime(),
                           vectorNamePattern, vectorFileFormat,
                           scalarNamePattern, scalarFileFormat,
                           (gOutputVerbosity >= NPFOV_FLOWS));
   if(success) {
      // ====== Tell passive node to start measurement ======================
      NetPerfMeterStartMessage startMsg;
      startMsg.Header.Type   = NETPERFMETER_START;
      startMsg.Header.Length = htons(sizeof(startMsg));
      startMsg.Padding       = 0x00000000;
      startMsg.MeasurementID = hton64(measurementID);
      startMsg.Header.Flags  = 0x00;
      if(hasSuffix(scalarNamePattern, ".bz2")) {
         startMsg.Header.Flags |= NPMSF_COMPRESS_SCALARS;
      }
      if(hasSuffix(vectorNamePattern, ".bz2")) {
         startMsg.Header.Flags |= NPMSF_COMPRESS_VECTORS;
      }

      sctp_sndrcvinfo sinfo;
      memset(&sinfo, 0, sizeof(sinfo));
      sinfo.sinfo_ppid = htonl(PPID_NETPERFMETER_CONTROL);

      if(gOutputVerbosity >= NPFOV_STATUS) {
         std::cout << "Starting measurement ... <S1> "; std::cout.flush();
      }
      if(sctp_send(controlSocket, &startMsg, sizeof(startMsg), &sinfo, 0) < 0) {
         return(false);
      }
      if(gOutputVerbosity >= NPFOV_STATUS) {
         std::cout << "<S2> "; std::cout.flush();
      }
      if(awaitNetPerfMeterAcknowledge(messageReader, controlSocket,
                                      measurementID, 0, 0) == false) {
         return(false);
      }
      if(gOutputVerbosity >= NPFOV_STATUS) {
         std::cout << "okay" << std::endl << std::endl;
      }
      return(true);
   }
   return(false);
}


// ###### Tell remote node to remove a flow #################################
static bool sendNetPerfMeterRemoveFlow(MessageReader* messageReader,
                                       int            controlSocket,
                                       Measurement*   measurement,
                                       Flow*          flow)
{
   flow->getVectorFile().finish(true);

   NetPerfMeterRemoveFlowMessage removeFlowMsg;
   removeFlowMsg.Header.Type   = NETPERFMETER_REMOVE_FLOW;
   removeFlowMsg.Header.Flags  = 0x00;
   removeFlowMsg.Header.Length = htons(sizeof(removeFlowMsg));
   removeFlowMsg.MeasurementID = hton64(flow->getMeasurementID());
   removeFlowMsg.FlowID        = htonl(flow->getFlowID());
   removeFlowMsg.StreamID      = htons(flow->getStreamID());

   sctp_sndrcvinfo sinfo;
   memset(&sinfo, 0, sizeof(sinfo));
   sinfo.sinfo_ppid = htonl(PPID_NETPERFMETER_CONTROL);

   if(gOutputVerbosity >= NPFOV_STATUS) {
      std::cout << "<flow " << flow->getFlowID() << "> "; std::cout.flush();
   }
   if(sctp_send(controlSocket, &removeFlowMsg, sizeof(removeFlowMsg), &sinfo, 0) <= 0) {
      return(false);
   }
   return(downloadResults(messageReader, controlSocket,
                          measurement->getVectorNamePattern(), flow));
}


// ###### Stop measurement ##################################################
bool performNetPerfMeterStop(MessageReader* messageReader,
                             int            controlSocket,
                             const uint64_t measurementID)
{
   // ====== Stop flows =====================================================
   FlowManager::getFlowManager()->lock();
   FlowManager::getFlowManager()->stopMeasurement(measurementID);
   Measurement* measurement = FlowManager::getFlowManager()->findMeasurement(measurementID);
   assert(measurement != NULL);
   measurement->writeScalarStatistics(getMicroTime());
   FlowManager::getFlowManager()->unlock();

   // ====== Tell passive node to stop measurement ==========================
   NetPerfMeterStopMessage stopMsg;
   stopMsg.Header.Type   = NETPERFMETER_STOP;
   stopMsg.Header.Flags  = 0x00;
   stopMsg.Header.Length = htons(sizeof(stopMsg));
   stopMsg.Padding       = 0x00000000;
   stopMsg.MeasurementID = hton64(measurementID);

   sctp_sndrcvinfo sinfo;
   memset(&sinfo, 0, sizeof(sinfo));
   sinfo.sinfo_ppid = htonl(PPID_NETPERFMETER_CONTROL);

   if(gOutputVerbosity >= NPFOV_STATUS) {
      std::cout << "Stopping measurement ... <S1> "; std::cout.flush();
   }
   if(sctp_send(controlSocket, &stopMsg, sizeof(stopMsg), &sinfo, 0) < 0) {
      return(false);
   }
   if(gOutputVerbosity >= NPFOV_STATUS) {
      std::cout << "<S2> "; std::cout.flush();
   }
   if(awaitNetPerfMeterAcknowledge(messageReader, controlSocket,
                                   measurementID, 0, 0) == false) {
      return(false);
   }

   // ====== Download passive node's vector file ============================
   const std::string vectorName = Flow::getNodeOutputName(
      measurement->getVectorNamePattern(), "passive");
   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << std::endl << "Downloading results [" << vectorName << "] ";
      std::cout.flush();
   }
   if(downloadOutputFile(messageReader, controlSocket, vectorName.c_str()) == false) {
      delete measurement;
      return(false);
   }
   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << " ";
   }

   // ====== Download passive node's scalar file ============================
   const std::string scalarName = Flow::getNodeOutputName(
      measurement->getScalarNamePattern(), "passive");
   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << std::endl << "Downloading results [" << scalarName << "] ";
      std::cout.flush();
   }
   if(downloadOutputFile(messageReader, controlSocket, scalarName.c_str()) == false) {
      delete measurement;
      return(false);
   }
   if(gOutputVerbosity >= NPFOV_STATUS) {
      std::cout << " okay" << std::endl;
   }

   // ====== Download flow results and remove the flows =====================
   FlowManager::getFlowManager()->lock();
   std::vector<Flow*>::iterator iterator = FlowManager::getFlowManager()->getFlowSet().begin();
   while(iterator != FlowManager::getFlowManager()->getFlowSet().end()) {
      Flow* flow = *iterator;
      if(flow->getMeasurementID() == measurementID) {
         if(sendNetPerfMeterRemoveFlow(messageReader, controlSocket,
                                       measurement, flow) == false) {
            delete measurement;
            return(false);
         }
         if(gOutputVerbosity >= NPFOV_FLOWS) {
            flow->print(std::cout, true);
         }
         delete flow;
         // Invalidated iterator. Is there a better solution?
         iterator = FlowManager::getFlowManager()->getFlowSet().begin();
         continue;
      }
      iterator++;
   }
   FlowManager::getFlowManager()->unlock();

   // ====== Remove the Measurement object =================================
   delete measurement;
   return(true);
}


// ###### Wait for NETPERFMETER_ACKNOWLEDGE from remote node ################
bool awaitNetPerfMeterAcknowledge(MessageReader* messageReader,
                                  int            controlSocket,
                                  const uint64_t measurementID,
                                  const uint32_t flowID,
                                  const uint16_t streamID,
                                  const int      timeout)
{
   // ====== Wait until there is something to read or a timeout =============
   struct pollfd pfd;
   pfd.fd      = controlSocket;
   pfd.events  = POLLIN;
   pfd.revents = 0;
   const int result = ext_poll_wrapper(&pfd, 1, timeout);
   if(result < 1) {
      if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
         std::cout << "<timeout> ";
         std::cout.flush();
      }
      return(false);
   }
   if(!(pfd.revents & POLLIN)) {
      if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
         std::cout << "<no answer> ";
      }
      return(false);
   }

   // ====== Read NETPERFMETER_ACKNOWLEDGE message ==========================
   NetPerfMeterAcknowledgeMessage ackMsg;
   ssize_t                        received;
   do {
      received = gMessageReader.receiveMessage(controlSocket, &ackMsg, sizeof(ackMsg));
   } while(received == MRRM_PARTIAL_READ);
   if(received < (ssize_t)sizeof(ackMsg)) {
      return(false);
   }
   if(ackMsg.Header.Type != NETPERFMETER_ACKNOWLEDGE) {
      std::cerr << "ERROR: Received message type " << (unsigned int)ackMsg.Header.Type
                << " instead of NETPERFMETER_ACKNOWLEDGE!" << std::endl;
      return(false);
   }

   // ====== Check whether NETPERFMETER_ACKNOWLEDGE is okay =================
   if( (ntoh64(ackMsg.MeasurementID) != measurementID) ||
       (ntohl(ackMsg.FlowID) != flowID) ||
       (ntohs(ackMsg.StreamID) != streamID) ) {
      std::cerr << "ERROR: Received NETPERFMETER_ACKNOWLEDGE for wrong measurement/flow/stream!"
                << std::endl;
      return(false);
   }

   const uint32_t status = ntohl(ackMsg.Status);
   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << "<status=" << status << "> ";
      std::cout.flush();
   }
   return(status == NETPERFMETER_STATUS_OKAY);
}



// ##########################################################################
// #### Passive Side Control                                             ####
// ##########################################################################


// ###### Upload file #######################################################
static bool uploadOutputFile(const int          controlSocket,
                             const sctp_assoc_t assocID,
                             const OutputFile&  outputFile)
{
   char                 messageBuffer[sizeof(NetPerfMeterResults) +
                                      NETPERFMETER_RESULTS_MAX_DATA_LENGTH];
   NetPerfMeterResults* resultsMsg = (NetPerfMeterResults*)&messageBuffer;

   // ====== Initialize header ==============================================
   sctp_sndrcvinfo sinfo;
   memset(&sinfo, 0, sizeof(sinfo));
   sinfo.sinfo_assoc_id    = assocID;
   sinfo.sinfo_ppid        = htonl(PPID_NETPERFMETER_CONTROL);
   resultsMsg->Header.Type = NETPERFMETER_RESULTS;

   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << std::endl << "Uploading results ";
      std::cout.flush();
   }

   // ====== Transmission loop ==============================================
   bool success = true;
   do {
      // ====== Read chunk from file ========================================
      const size_t bytes = fread(&resultsMsg->Data, 1,
                                 NETPERFMETER_RESULTS_MAX_DATA_LENGTH,
                                 outputFile.getFile());
      resultsMsg->Header.Flags  = feof(outputFile.getFile()) ? NPMRF_EOF : 0x00;
      resultsMsg->Header.Length = htons(sizeof(NetPerfMeterResults) + bytes);
      // printf("   -> b=%d   snd=%d\n",(int)bytes, (int)(sizeof(NetPerfMeterResults) + bytes));
      if(ferror(outputFile.getFile())) {
         std::cerr << "ERROR: Failed to read results from "
                   << outputFile.getName() << " - "
                   << strerror(errno) << "!" << std::endl;
         success = false;
         break;
      }

      // ====== Transmit chunk ==============================================
      if(sctp_send(controlSocket, resultsMsg, sizeof(NetPerfMeterResults) + bytes, &sinfo, 0) < 0) {
         std::cerr << "ERROR: Failed to upload results - " << strerror(errno) << "!" << std::endl;
         success = false;
         break;
      }
      if(gOutputVerbosity >= NPFOV_REALLY_VERBOSE) {
         std::cout << ".";
      }
      std::cout.flush();
   } while(!(resultsMsg->Header.Flags & NPMRF_EOF));

   // ====== Check results ==================================================
   if(!success) {
      std::cerr << std::endl
                << "WARNING: Unable to upload results (errno=" << errno << "): "
                << strerror(errno) << std::endl;
      sendAbort(controlSocket, assocID);
   }
   if(gOutputVerbosity >= NPFOV_CONNECTIONS) {
      std::cout << " " << ((success == true) ? "okay": "FAILED") << std::endl;
   }
   return(success);
}


// ###### Upload per-flow statistics file ###################################
static bool uploadResults(const int          controlSocket,
                          const sctp_assoc_t assocID,
                          Flow*              flow)
{
   bool success = flow->getVectorFile().finish(false);
   if(success) {
      success = sendNetPerfMeterAcknowledge(
                   controlSocket, assocID,
                   flow->getMeasurementID(), flow->getFlowID(), flow->getStreamID(),
                   (success == true) ? NETPERFMETER_STATUS_OKAY :
                                       NETPERFMETER_STATUS_ERROR);
      if(success) {
         success = uploadOutputFile(controlSocket, assocID, flow->getVectorFile());
      }
   }
   flow->getVectorFile().finish(true);
   return(success);
}


// ###### Handle NETPERFMETER_ADD_FLOW ######################################
static bool handleNetPerfMeterAddFlow(MessageReader*                    messageReader,
                                      const int                         controlSocket,
                                      const NetPerfMeterAddFlowMessage* addFlowMsg,
                                      const sctp_assoc_t                assocID,
                                      const size_t                      received)
{
   if(received < sizeof(NetPerfMeterAddFlowMessage)) {
      std::cerr << "ERROR: Received malformed NETPERFMETER_ADD_FLOW control message!"
                << std::endl;
      return(false);
   }
   const uint64_t measurementID   = ntoh64(addFlowMsg->MeasurementID);
   const uint32_t flowID          = ntohl(addFlowMsg->FlowID);
   const uint16_t streamID        = ntohs(addFlowMsg->StreamID);
   const size_t   startStopEvents = ntohs(addFlowMsg->OnOffEvents);
   if(received < sizeof(NetPerfMeterAddFlowMessage) + (startStopEvents * sizeof(unsigned int))) {
      std::cerr << "ERROR: Received malformed NETPERFMETER_ADD_FLOW control message "
                   "(too few start/stop entries)!" << std::endl;
      return(false);
   }
   char description[sizeof(addFlowMsg->Description) + 1];
   memcpy((char*)&description, (const char*)&addFlowMsg->Description, sizeof(addFlowMsg->Description));
   description[sizeof(addFlowMsg->Description)] = 0x00;
   if(FlowManager::getFlowManager()->findFlow(measurementID, flowID, streamID)) {
      std::cerr << "ERROR: NETPERFMETER_ADD_FLOW tried to add already-existing flow!"
                << std::endl;
      return(sendNetPerfMeterAcknowledge(controlSocket, assocID,
                                         measurementID, flowID, streamID,
                                         NETPERFMETER_STATUS_ERROR));
   }
   else {
      // ====== Create new flow =============================================
      FlowTrafficSpec trafficSpec;
      trafficSpec.Protocol             = addFlowMsg->Protocol;
      trafficSpec.Description          = std::string(description);
      for(size_t i = 0;i < NETPERFMETER_RNG_INPUT_PARAMETERS;i++) {
         trafficSpec.OutboundFrameRate[i] = networkToDouble(addFlowMsg->FrameRate[i]);
         trafficSpec.OutboundFrameSize[i] = networkToDouble(addFlowMsg->FrameSize[i]);
      }
      trafficSpec.OutboundFrameRateRng     = addFlowMsg->FrameRateRng;
      trafficSpec.OutboundFrameSizeRng     = addFlowMsg->FrameSizeRng;
      trafficSpec.MaxMsgSize               = ntohs(addFlowMsg->MaxMsgSize);
      trafficSpec.RcvBufferSize            = ntohl(addFlowMsg->RcvBufferSize);
      trafficSpec.SndBufferSize            = ntohl(addFlowMsg->SndBufferSize);
      trafficSpec.OrderedMode              = ntohl(addFlowMsg->OrderedMode)  / (double)0xffffffff;
      trafficSpec.ReliableMode             = ntohl(addFlowMsg->ReliableMode) / (double)0xffffffff;
      trafficSpec.RetransmissionTrials     = ntohl(addFlowMsg->RetransmissionTrials) & ~NPMAF_RTX_TRIALS_IN_MILLISECONDS;
      trafficSpec.RetransmissionTrialsInMS = (ntohl(addFlowMsg->RetransmissionTrials) & NPMAF_RTX_TRIALS_IN_MILLISECONDS);
      if( (trafficSpec.RetransmissionTrialsInMS) && (trafficSpec.RetransmissionTrials == 0x7fffffff) ) {
         trafficSpec.RetransmissionTrials = ~0;
      }
      for(size_t i = 0;i < startStopEvents;i++) {
         trafficSpec.OnOffEvents.insert(ntohl(addFlowMsg->OnOffEvent[i]));
      }
      
      Flow* flow = new Flow(ntoh64(addFlowMsg->MeasurementID), ntohl(addFlowMsg->FlowID),
                            ntohs(addFlowMsg->StreamID), trafficSpec,
                            controlSocket, assocID);
      return(sendNetPerfMeterAcknowledge(controlSocket, assocID,
                                         measurementID, flowID, streamID,
                                         (flow != NULL) ? NETPERFMETER_STATUS_OKAY :
                                                          NETPERFMETER_STATUS_ERROR));
   }
}


// ###### Handle NETPERFMETER_REMOVE_FLOW #####################################
static bool handleNetPerfMeterRemoveFlow(MessageReader*                       messageReader,
                                         const int                            controlSocket,
                                         const NetPerfMeterRemoveFlowMessage* removeFlowMsg,
                                         const sctp_assoc_t                   assocID,
                                         const size_t                         received)
{
   if(received < sizeof(NetPerfMeterRemoveFlowMessage)) {
      std::cerr << "ERROR: Received malformed NETPERFMETER_REMOVE_FLOW control message!"
                << std::endl;
      return(false);
   }
   const uint64_t measurementID = ntoh64(removeFlowMsg->MeasurementID);
   const uint32_t flowID        = ntohl(removeFlowMsg->FlowID);
   const uint16_t streamID      = ntohs(removeFlowMsg->StreamID);
   Flow* flow = FlowManager::getFlowManager()->findFlow(measurementID, flowID, streamID);
   if(flow == NULL) {
      std::cerr << "ERROR: NETPERFMETER_ADD_REMOVE tried to remove not-existing flow!"
                << std::endl;
      return(sendNetPerfMeterAcknowledge(controlSocket, assocID,
                                         measurementID, flowID, streamID,
                                         NETPERFMETER_STATUS_ERROR));
   }
   else {
      // ------ Remove Flow from Flow Manager -------------------------
      FlowManager::getFlowManager()->removeFlow(flow);
      // ------ Upload statistics file --------------------------------
      flow->getVectorFile().finish(false);
      uploadResults(controlSocket, assocID, flow);
      delete flow;
   }
   return(true);
}


// ###### Handle NETPERFMETER_START #########################################
static bool handleNetPerfMeterStart(MessageReader*                  messageReader,
                                    const int                       controlSocket,
                                    const NetPerfMeterStartMessage* startMsg,
                                    const sctp_assoc_t              assocID,
                                    const size_t                    received)
{
   if(received < sizeof(NetPerfMeterStartMessage)) {
      std::cerr << "ERROR: Received malformed NETPERFMETER_START control message!" << std::endl;
      return(false);
   }
   const uint64_t measurementID = ntoh64(startMsg->MeasurementID);

   std::cout << std::endl << "Starting measurement "
               << format("$%llx", (unsigned long long)measurementID) << " ..." << std::endl;

   OutputFileFormat scalarFileFormat = OFF_Plain;
   if(startMsg->Header.Flags & NPMSF_COMPRESS_SCALARS) {
      scalarFileFormat = OFF_BZip2;
   }
   if(startMsg->Header.Flags & NPMSF_NO_SCALARS) {
      scalarFileFormat = OFF_None;
   }
   OutputFileFormat vectorFileFormat = OFF_Plain;
   if(startMsg->Header.Flags & NPMSF_COMPRESS_VECTORS) {
      vectorFileFormat = OFF_BZip2;
   }
   if(startMsg->Header.Flags & NPMSF_NO_VECTORS) {
      vectorFileFormat = OFF_None;
   }

   const unsigned long long now = getMicroTime();
   bool success = FlowManager::getFlowManager()->startMeasurement(
      measurementID, now,
      NULL, vectorFileFormat,
      NULL, scalarFileFormat,
      (gOutputVerbosity >= NPFOV_FLOWS));

   return(sendNetPerfMeterAcknowledge(controlSocket, assocID,
                                      measurementID, 0, 0,
                                      (success == true) ? NETPERFMETER_STATUS_OKAY :
                                                          NETPERFMETER_STATUS_ERROR));
}


// ###### Handle NETPERFMETER_STOP #########################################
static bool handleNetPerfMeterStop(MessageReader*                 messageReader,
                                   const int                      controlSocket,
                                   const NetPerfMeterStopMessage* stopMsg,
                                   const sctp_assoc_t             assocID,
                                   const size_t                   received)
{
   if(received < sizeof(NetPerfMeterStopMessage)) {
      std::cerr << "ERROR: Received malformed NETPERFMETER_STOP control message!"
                << std::endl;
      return(false);
   }
   const uint64_t measurementID = ntoh64(stopMsg->MeasurementID);

   std::cout << std::endl << "Stopping measurement "
             << format("$%llx", (unsigned long long)measurementID)
             << " ..." << std::endl;

   // ====== Stop flows =====================================================
   FlowManager::getFlowManager()->lock();
   FlowManager::getFlowManager()->stopMeasurement(measurementID, (gOutputVerbosity >= NPFOV_FLOWS));
   bool         success     = false;
   Measurement* measurement =
      FlowManager::getFlowManager()->findMeasurement(measurementID);
   if(measurement) {
      measurement->lock();
      measurement->writeScalarStatistics(getMicroTime());
      success = measurement->finish(false);
      measurement->unlock();
   }
   
   // ====== Remove pointer to Measurement from its flows ===================
   for(std::vector<Flow*>::iterator iterator = FlowManager::getFlowManager()->getFlowSet().begin();
      iterator != FlowManager::getFlowManager()->getFlowSet().end();
      iterator++) {
      Flow* flow = *iterator;
      if(flow->getMeasurement() == measurement) {
         flow->setMeasurement(NULL);
      }
   }
   FlowManager::getFlowManager()->unlock();
      
   // ====== Acknowledge result =============================================
   sendNetPerfMeterAcknowledge(controlSocket, assocID,
                               measurementID, 0, 0,
                               (success == true) ? NETPERFMETER_STATUS_OKAY : 
                                                   NETPERFMETER_STATUS_ERROR);
                                                   
   // ====== Upload results =================================================
   if(measurement) {
      if(success) {
         uploadOutputFile(controlSocket, assocID, measurement->getVectorFile());
         uploadOutputFile(controlSocket, assocID, measurement->getScalarFile());
      }
      delete measurement;
   }
   return(true);
}   


// ###### Delete all flows owned by a given remote node #####################
static void handleControlAssocShutdown(const sctp_assoc_t assocID)
{
   FlowManager::getFlowManager()->lock();

   std::vector<Flow*>::iterator iterator = FlowManager::getFlowManager()->getFlowSet().begin();
   while(iterator != FlowManager::getFlowManager()->getFlowSet().end()) {
      Flow* flow = *iterator;
      if(flow->getRemoteControlAssocID() == assocID) {
         Measurement* measurement = flow->getMeasurement();
         if(measurement) {
            delete measurement;
         }
         delete flow;
         // Invalidated iterator. Is there a better solution?         
         iterator = FlowManager::getFlowManager()->getFlowSet().begin();
         continue;
      }
      iterator++;
   }
   
   FlowManager::getFlowManager()->unlock();
}


// ###### Handle incoming control message ###################################
bool handleNetPerfMeterControlMessage(MessageReader* messageReader,
                                      int            controlSocket)
{
   char            inputBuffer[65536];
   sockaddr_union  from;
   socklen_t       fromlen = sizeof(from);
   int             flags   = 0;
   sctp_sndrcvinfo sinfo;

   // ====== Read message (or fragment) =====================================
   const ssize_t received =
      messageReader->receiveMessage(controlSocket, &inputBuffer, sizeof(inputBuffer),
                                    &from.sa, &fromlen, &sinfo, &flags);
   if(received == MRRM_PARTIAL_READ) {
      return(true);   // Partial read -> wait for next fragment.
   }
   else if(received < (ssize_t)sizeof(NetPerfMeterHeader)) {
      std::cerr << "ERROR: Control connection is broken!" << std::endl;
      sendAbort(controlSocket, sinfo.sinfo_assoc_id);
      return(false);
   }

   // ====== Received a SCTP notification ===================================
   if(flags & MSG_NOTIFICATION) {
      const sctp_notification* notification = (const sctp_notification*)&inputBuffer;
      if( (notification->sn_header.sn_type == SCTP_ASSOC_CHANGE) &&
          ((notification->sn_assoc_change.sac_state == SCTP_COMM_LOST) ||
           (notification->sn_assoc_change.sac_state == SCTP_SHUTDOWN_COMP)) ) {
        handleControlAssocShutdown(notification->sn_assoc_change.sac_assoc_id);
      }
   }

   // ====== Received a real control message ================================
   else {
      const NetPerfMeterHeader* header = (const NetPerfMeterHeader*)&inputBuffer;
      if(ntohs(header->Length) != received) {
         std::cerr << "ERROR: Received malformed control message!" << std::endl
            << "       expected=" << ntohs(header->Length)
            << ", received="<< received << std::endl;
         return(false);
      }

      switch(header->Type) {
         case NETPERFMETER_ADD_FLOW:
            return(handleNetPerfMeterAddFlow(
                      messageReader, controlSocket,
                      (const NetPerfMeterAddFlowMessage*)&inputBuffer,
                      sinfo.sinfo_assoc_id, received));
          break;
         case NETPERFMETER_REMOVE_FLOW:
            return(handleNetPerfMeterRemoveFlow(
                      messageReader, controlSocket,
                      (const NetPerfMeterRemoveFlowMessage*)&inputBuffer,
                      sinfo.sinfo_assoc_id, received));
          break;
         case NETPERFMETER_START:
            return(handleNetPerfMeterStart(
                      messageReader, controlSocket,
                      (const NetPerfMeterStartMessage*)&inputBuffer,
                      sinfo.sinfo_assoc_id, received));
          break;
         case NETPERFMETER_STOP:
            return(handleNetPerfMeterStop(
                      messageReader, controlSocket,
                      (const NetPerfMeterStopMessage*)&inputBuffer,
                      sinfo.sinfo_assoc_id, received));
          break;
         default:
            std::cerr << "ERROR: Received invalid control message of type "
                     << (unsigned int)header->Type << "!" << std::endl;
            sendAbort(controlSocket, sinfo.sinfo_assoc_id);
            return(false);
          break;
      }
   }
   return(true);
}


// ###### Handle NETPERFMETER_IDENTIFY_FLOW message #########################
void handleNetPerfMeterIdentify(const NetPerfMeterIdentifyMessage* identifyMsg,
                                const int                          sd,
                                const sockaddr_union*              from)
{
   int          controlSocketDescriptor;
   sctp_assoc_t controlAssocID;
   Flow*        flow;

   OutputFileFormat vectorFileFormat = OFF_Plain;
   if(identifyMsg->Header.Flags & NPMSF_COMPRESS_VECTORS) {
      vectorFileFormat = OFF_BZip2;
   }
   if(identifyMsg->Header.Flags & NPMSF_NO_VECTORS) {
      vectorFileFormat = OFF_None;
   }

   flow = FlowManager::getFlowManager()->identifySocket(ntoh64(identifyMsg->MeasurementID),
                                                        ntohl(identifyMsg->FlowID),
                                                        ntohs(identifyMsg->StreamID),
                                                        sd, from,
                                                        vectorFileFormat,
                                                        controlSocketDescriptor,
                                                        controlAssocID);
   if((controlAssocID != 0) && (flow != NULL)) {
      bool success    = true;
      int  bufferSize = flow->getTrafficSpec().RcvBufferSize;
      if(ext_setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) < 0) {
         std::cerr << "ERROR: Failed to configure receive buffer size on SCTP socket (SO_RCVBUF option) - "
                   << strerror(errno) << "!" << std::endl;
         success = false;
      }
      bufferSize = flow->getTrafficSpec().SndBufferSize;
      if(ext_setsockopt(sd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0) {
         std::cerr << "ERROR: Failed to configure send buffer size on SCTP socket (SO_SNDBUF option) - "
                   << strerror(errno) << "!" << std::endl;
         success = false;
      }
      sendNetPerfMeterAcknowledge(controlSocketDescriptor, controlAssocID,
                                  ntoh64(identifyMsg->MeasurementID),
                                  ntohl(identifyMsg->FlowID),
                                  ntohs(identifyMsg->StreamID),
                                  (success == true) ? NETPERFMETER_STATUS_OKAY :
                                                      NETPERFMETER_STATUS_ERROR);
   }
   else {
      // No known flow -> no assoc ID to send response to!
      std::cerr << "WARNING: NETPERFMETER_IDENTIFY_FLOW message for unknown flow!" << std::endl;
   }
}


// ###### Send NETPERFMETER_ACKNOWLEDGE to remote node ######################
bool sendNetPerfMeterAcknowledge(int            controlSocket,
                                 sctp_assoc_t   assocID,
                                 const uint64_t measurementID,
                                 const uint32_t flowID,
                                 const uint16_t streamID,
                                 const uint32_t status)
{
   NetPerfMeterAcknowledgeMessage ackMsg;
   ackMsg.Header.Type   = NETPERFMETER_ACKNOWLEDGE;
   ackMsg.Header.Flags  = 0x00;
   ackMsg.Header.Length = htons(sizeof(ackMsg));
   ackMsg.MeasurementID = hton64(measurementID);
   ackMsg.FlowID        = htonl(flowID);
   ackMsg.StreamID      = htons(streamID);
   ackMsg.Padding       = 0x0000;
   ackMsg.Status        = htonl(status);

   sctp_sndrcvinfo sinfo;
   memset(&sinfo, 0, sizeof(sinfo));
   sinfo.sinfo_assoc_id = assocID;
   sinfo.sinfo_ppid     = htonl(PPID_NETPERFMETER_CONTROL);

   return(sctp_send(controlSocket, &ackMsg, sizeof(ackMsg), &sinfo, 0) > 0);
}
