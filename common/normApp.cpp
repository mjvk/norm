
// normApp.cpp - Command-line NORM application

#include "protokit.h"
#include "normSession.h"
#include "normPostProcess.h"

#include <stdio.h>   // for stdout/stderr printouts
#include <signal.h>  // for SIGTERM/SIGINT handling
#include <errno.h>
#include <stdlib.h>

// Command-line application using Protolib EventDispatcher
class NormApp : public NormController, public ProtoApp
{
    public:
        NormApp();
        virtual ~NormApp();
        
        // Overrides from ProtoApp or NsProtoSimAgent base
        bool OnStartup(int argc, const char*const* argv);
        bool ProcessCommands(int argc, const char*const* argv);
        void OnShutdown();
        
        bool ProcessCommand(const char* cmd, const char* val);
        
        static void DoInputReady(ProtoDispatcher::Descriptor descriptor, 
                                 ProtoDispatcher::Event      theEvent, 
                                 const void*                 userData);
        
    private:
        void OnInputReady();
            
        enum CmdType {CMD_INVALID, CMD_NOARG, CMD_ARG};
        CmdType CommandType(const char* cmd);
    
        virtual void Notify(NormController::Event event,
                        class NormSessionMgr* sessionMgr,
                        class NormSession*    session,
                        class NormServerNode* server,
                        class NormObject*     object);
        
        bool OnIntervalTimeout(ProtoTimer& theTimer);
    
        static const char* const cmd_list[];
        
#ifdef UNIX
        static void SignalHandler(int sigNum);
#endif // UNIX
            
        NormSessionMgr      session_mgr;
        NormSession*        session;
        NormStreamObject*   tx_stream;
        NormStreamObject*   rx_stream;
        
        // application parameters
        FILE*               input;  // input stream
        FILE*               output; // output stream
        char                input_buffer[1250];
        unsigned int        input_index;
        unsigned int        input_length;
        bool                input_active;
        bool                push_stream;
        bool                input_messaging; // stream input mode   
        UINT16              input_msg_length;
        UINT16              input_msg_index;
        char                output_buffer[65536];
        UINT16              output_index;
        bool                output_messaging;
        UINT16              output_msg_length;
        bool                output_msg_sync;
        
        // NormSession common parameters 
        char*               address;        // session address
        UINT16              port;           // session port number
        UINT8               ttl;
        double              tx_rate;        // bits/sec
        bool                cc_enable;
        
        // NormSession server-only parameters
        UINT16              segment_size;
        UINT8               ndata;
        UINT8               nparity;
        UINT8               auto_parity;
        UINT8               extra_parity;
        double              backoff_factor;
        unsigned long       tx_buffer_size; // bytes
        NormFileList        tx_file_list;
        double              tx_object_interval;
        int                 tx_repeat_count;
        double              tx_repeat_interval;
        ProtoTimer          interval_timer;
        
        // NormSession client-only parameters
        unsigned long       rx_buffer_size; // bytes
        NormFileList        rx_file_cache;
        char*               rx_cache_path;
        NormPostProcessor*  post_processor;
        bool                unicast_nacks;
        bool                silent_client;
        
        // Debug parameters
        bool                tracing;
        double              tx_loss;
        double              rx_loss;
    
}; // end class NormApp

NormApp::NormApp()
 : session_mgr(GetTimerMgr(), GetSocketNotifier()),
   session(NULL), tx_stream(NULL), rx_stream(NULL), input(NULL), output(NULL), 
   input_index(0), input_length(0), input_active(false),
   push_stream(false), input_messaging(false), input_msg_length(0), input_msg_index(0),
   output_index(0), output_messaging(false), output_msg_length(0), output_msg_sync(false),
   address(NULL), port(0), ttl(3), tx_rate(64000.0), cc_enable(false),
   segment_size(1024), ndata(32), nparity(16), auto_parity(0), extra_parity(0),
   backoff_factor(NormSession::DEFAULT_BACKOFF_FACTOR),
   tx_buffer_size(1024*1024), 
   tx_object_interval(0.0), tx_repeat_count(0), tx_repeat_interval(0.0),
   rx_buffer_size(1024*1024), rx_cache_path(NULL), unicast_nacks(false), silent_client(false),
   tracing(false), tx_loss(0.0), rx_loss(0.0)
{
    // Init tx_timer for 1.0 second interval, infinite repeats
     session_mgr.SetController(this);
    
    interval_timer.SetListener(this, &NormApp::OnIntervalTimeout);
    interval_timer.SetInterval(0.0);
    interval_timer.SetRepeat(0);
    
    struct timeval currentTime;
    ProtoSystemTime(currentTime);
    srand(currentTime.tv_usec);
}

NormApp::~NormApp()
{
    if (address) delete address;
    if (rx_cache_path) delete rx_cache_path;
    if (post_processor) delete post_processor;
}


const char* const NormApp::cmd_list[] = 
{
    "+debug",        // debug level
    "+log",          // log file name
    "-trace",        // message tracing on
    "+txloss",       // tx packet loss percent (for testing)
    "+rxloss",       // rx packet loss percent (for testing)
    "+address",      // session destination address
    "+ttl",          // multicast hop count scope
    "+cc",           // congestion control on/off
    "+rate",         // tx date rate (bps)
    "-push",         // push stream writes for real-time messaging
    "+input",        // send stream input
    "+output",       // recv stream output
    "+minput",       // send message stream input
    "+moutput",      // recv message stream output
    "+sendfile",     // file/directory list to transmit
    "+interval",     // delay time (sec) between files (0.0 sec default)
    "+repeatcount",  // How many times to repeat the file/directory list
    "+rinterval",    // Interval (sec) between file/directory list repeats
    "+rxcachedir",   // recv file cache directory
    "+segment",      // payload segment size (bytes)
    "+block",        // User data packets per FEC coding block (blockSize)
    "+parity",       // FEC packets calculated per coding block (nparity)
    "+auto",         // Number of FEC packets to proactively send (<= nparity)
    "+extra",        // Number of extra FEC packets sent in response to repair requests
    "+backoff",      // Backoff factor to use
    "+txbuffer",     // Size of sender's buffer
    "+rxbuffer",     // Size receiver allocates for buffering each sender
    "-unicastNacks", // unicast instead of multicast feedback messages
    "-silentClient", // "silent" (non-nacking) client (EMCON mode)
    "+processor",    // receive file post processing command
    NULL         
};
    
bool NormApp::ProcessCommand(const char* cmd, const char* val)
{
    CmdType type = CommandType(cmd);
    ASSERT(CMD_INVALID != type);
    unsigned int len = strlen(cmd);
    if ((CMD_ARG == type) && !val)
    {
        DMSG(0, "NormApp::ProcessCommand(%s) missing argument\n", cmd);
        return false;        
    }
    
    if (!strncmp("debug", cmd, len))
    {
        
        int debugLevel = atoi(val);
        if ((debugLevel < 0) || (debugLevel > 12))
        {
            DMSG(0, "NormApp::ProcessCommand(segment) invalid debug level!\n");   
            return false;
        }
        SetDebugLevel(debugLevel);
    }
    else if (!strncmp("log", cmd, len))
    {
        OpenDebugLog(val);
    }
    else if (!strncmp("trace", cmd, len))
    {
        tracing = true;
        if (session) session->SetTrace(true);
    }
    else if (!strncmp("txloss", cmd, len))
    {
        double txLoss = atof(val);
        if (txLoss < 0)
        {
            DMSG(0, "NormApp::ProcessCommand(txloss) invalid txRate!\n");   
            return false;
        }
        tx_loss = txLoss;
        if (session) session->SetTxLoss(txLoss);
    }
    else if (!strncmp("rxloss", cmd, len))
    {
        double rxLoss = atof(val);
        if (rxLoss < 0)
        {
            DMSG(0, "NormApp::ProcessCommand(rxloss) invalid txRate!\n");   
            return false;
        }
        rx_loss = rxLoss;
        if (session) session->SetRxLoss(rxLoss);
    }
    else if (!strncmp("address", cmd, len))
    {
        unsigned int len = strlen(val);
        if (address) delete address;
        if (!(address = new char[len+1]))
        {
            DMSG(0, "NormApp::ProcessCommand(address) allocation error:%s\n",
                strerror(errno)); 
            return false;
        }
        strcpy(address, val);
        char* ptr = strchr(address, '/');
        if (!ptr)
        {
            delete address;
            address = NULL;
            DMSG(0, "NormApp::ProcessCommand(address) missing port number!\n");   
            return false;
        }
        *ptr++ = '\0';
        int portNum = atoi(ptr);
        if ((portNum < 1) || (portNum > 65535))
        {
            delete address;
            address = NULL;
            DMSG(0, "NormApp::ProcessCommand(address) invalid port number!\n");   
            return false;
        }
        port = portNum;
    }
    else if (!strncmp("ttl", cmd, len))
    {
        int ttlTemp = atoi(val);
        if ((ttlTemp < 1) || (ttlTemp > 255))
        {
            DMSG(0, "NormApp::ProcessCommand(ttl) invalid value!\n");   
            return false;
        }
        ttl = ttlTemp;
    }
    else if (!strncmp("rate", cmd, len))
    {
        double txRate = atof(val);
        if (txRate < 0)
        {
            DMSG(0, "NormApp::ProcessCommand(rate) invalid txRate!\n");   
            return false;
        }
        tx_rate = txRate;
        if (session) session->SetTxRate(txRate);
    }
    else if (!strncmp("cc", cmd, len))
    {
        if (!strcmp("on", val))
        {
            cc_enable = true;
        }
        else if (!strcmp("off", val))
        {
            cc_enable = false;   
        }
        else
        {
            DMSG(0, "NormApp::ProcessCommand(cc) invalid option!\n");   
            return false;
        }
        if (session) session->SetCongestionControl(cc_enable);
    }
    else if (!strncmp("input", cmd, len))
    {
        if (!strcmp(val, "STDIN"))
	    {
            input = stdin;
	    }
        else if (!(input = fopen(val, "rb")))
        {
            DMSG(0, "NormApp::ProcessCommand(input) fopen() error: %s\n",
                    strerror(errno));
            return false;   
        }
        input_index = input_length = 0;
        input_messaging = false;
#ifdef UNIX
        // Set input non-blocking
        if(-1 == fcntl(fileno(input), F_SETFL, fcntl(fileno(input), F_GETFL, 0) | O_NONBLOCK))
            perror("NormApp::ProcessCommand(input) fcntl(F_SETFL(O_NONBLOCK)) error");
#endif // UNIX
    }
    else if (!strncmp("output", cmd, len))
    {
        if (!strcmp(val, "STDOUT"))
	    {
            output = stdout;
	    }
        else if (!(output = fopen(val, "wb")))
        {
            DMSG(0, "NormApp::ProcessCommand(output) fopen() error: %s\n",
                    strerror(errno));
            return false;   
        }
        output_messaging = false;
        output_index = 0;
        output_msg_sync = true;
    }
    else if (!strncmp("minput", cmd, len))
    {
        if (!strcmp(val, "STDIN"))
	    {
            input = stdin;
	    }
        else if (!(input = fopen(val, "rb")))
        {
            DMSG(0, "NormApp::ProcessCommand(input) fopen() error: %s\n",
                    strerror(errno));
            return false;   
        }
        input_index = input_length = 0;
        input_messaging = true;
#ifdef UNIX
        // Set input non-blocking
        if(-1 == fcntl(fileno(input), F_SETFL, fcntl(fileno(input), F_GETFL, 0) | O_NONBLOCK))
            perror("NormApp::ProcessCommand(input) fcntl(F_SETFL(O_NONBLOCK)) error");
#endif // UNIX
    }
    else if (!strncmp("moutput", cmd, len))
    {
        if (!strcmp(val, "STDOUT"))
	    {
            output = stdout;
	    }
        else if (!(output = fopen(val, "wb")))
        {
            DMSG(0, "NormApp::ProcessCommand(output) fopen() error: %s\n",
                    strerror(errno));
            return false;   
        }
        output_messaging = true;
        output_msg_length = output_index = 0;
        output_msg_sync = false;
    }
    else if (!strncmp("sendfile", cmd, len))
    {
        if (!tx_file_list.Append(val))
        {
            DMSG(0, "NormApp::ProcessCommand(sendfile) Error appending \"%s\" "
                    "to tx file list.\n", val);
            return false;   
        }
    }
    else if (!strncmp("interval", cmd, len))
    {
        if (1 != sscanf(val, "%lf", &tx_object_interval)) 
            tx_object_interval = -1.0;
        if (tx_object_interval < 0.0)
        {
            DMSG(0, "NormApp::ProcessCommand(interval) Invalid tx object interval: %s\n",
                     val);
            tx_object_interval = 0.0;
            return false;
        }
    }    
    else if (!strncmp("repeat", cmd, len))
    {
        tx_repeat_count = atoi(val);  
    }
    else if (!strncmp("rinterval", cmd, len))
    {
        if (1 != sscanf(val, "%lf", &tx_repeat_interval)) 
            tx_repeat_interval = -1.0;
        if (tx_repeat_interval < 0.0)
        {
            DMSG(0, "NormApp::ProcessCommand(rinterval) Invalid tx repeat interval: %s\n",
                     val);
            tx_repeat_interval = 0.0;
            return false;
        }
    }   
    else if (!strncmp("rxcachedir", cmd, len))
    {
        unsigned int length = strlen(val);   
        // Make sure there is a trailing PROTO_PATH_DELIMITER
        if (PROTO_PATH_DELIMITER != val[length-1]) 
            length += 2;
        else
            length += 1;
        if (!(rx_cache_path = new char[length]))
        {
             DMSG(0, "NormApp::ProcessCommand(rxcachedir) alloc error: %s\n",
                    strerror(errno));
            return false;  
        }
        strcpy(rx_cache_path, val);
        rx_cache_path[length-2] = PROTO_PATH_DELIMITER;
        rx_cache_path[length-1] = '\0';
    }
    else if (!strncmp("segment", cmd, len))
    {
        int segmentSize = atoi(val);
        if ((segmentSize < 0) || (segmentSize > 8000))
        {
            DMSG(0, "NormApp::ProcessCommand(segment) invalid segment size!\n");   
            return false;
        }
        segment_size = segmentSize;
    }
    else if (!strncmp("block", cmd, len))
    {
        int blockSize = atoi(val);
        if ((blockSize < 1) || (blockSize > 255))
        {
            DMSG(0, "NormApp::ProcessCommand(block) invalid block size!\n");   
            return false;
        }
        ndata = blockSize;
    }
    else if (!strncmp("parity", cmd, len))
    {
        int numParity = atoi(val);
        if ((numParity < 0) || (numParity > 254))
        {
            DMSG(0, "NormApp::ProcessCommand(parity) invalid value!\n");   
            return false;
        }
        nparity = numParity;
    }
    else if (!strncmp("auto", cmd, len))
    {
        int autoParity = atoi(val);
        if ((autoParity < 0) || (autoParity > 254))
        {
            DMSG(0, "NormApp::ProcessCommand(auto) invalid value!\n");   
            return false;
        }
        auto_parity = autoParity;
        if (session) session->ServerSetAutoParity(autoParity);
    }
    else if (!strncmp("extra", cmd, len))
    {
        int extraParity = atoi(val);
        if ((extraParity < 0) || (extraParity > 254))
        {
            DMSG(0, "NormApp::ProcessCommand(extra) invalid value!\n");   
            return false;
        }
        extra_parity = extraParity;
        if (session) session->ServerSetExtraParity(extraParity);
    }
    else if (!strncmp("backoff", cmd, len))
    {
        double backoffFactor = atof(val);
        if (backoffFactor < 0)
        {
            DMSG(0, "NormSimAgent::ProcessCommand(backoff) invalid txRate!\n");   
            return false;
        }
        backoff_factor = backoffFactor;
        if (session) session->SetBackoffFactor(backoffFactor);
    }
    else if (!strncmp("txbuffer", cmd, len))
    {
        if (1 != sscanf(val, "%lu", &tx_buffer_size))
        {
            DMSG(0, "NormApp::ProcessCommand(txbuffer) invalid value!\n");   
            return false;
        }
    }
    else if (!strncmp("rxbuffer", cmd, len))
    {
        if (1 != sscanf(val, "%lu", &rx_buffer_size))
        {
            DMSG(0, "NormApp::ProcessCommand(rxbuffer) invalid value!\n");   
            return false;
        }
    }
    else if (!strncmp("unicastNacks", cmd, len))
    {
        unicast_nacks = true;
        if (session) session->SetUnicastNacks(true);
    }
    else if (!strncmp("silentClient", cmd, len))
    {
        silent_client = true;
        if (session) session->ClientSetSilent(true);
    }
    else if (!strncmp("push", cmd, len))
    {
        push_stream = true;
    }
    else if (!strncmp("processor", cmd, len))
    {
        if (!post_processor->SetCommand(val))
        {
            DMSG(0, "NormApp::ProcessCommand(processor) error!\n");   
            return false;
        }
    }
    return true;
}  // end NormApp::ProcessCommand()


NormApp::CmdType NormApp::CommandType(const char* cmd)
{
    if (!cmd) return CMD_INVALID;
    unsigned int len = strlen(cmd);
    bool matched = false;
    CmdType type = CMD_INVALID;
    const char* const* nextCmd = cmd_list;
    while (*nextCmd)
    {
        if (!strncmp(cmd, *nextCmd+1, len))
        {
            if (matched)
            {
                // ambiguous command (command should match only once)
                return CMD_INVALID;
            }
            else
            {
                matched = true;   
                if ('+' == *nextCmd[0])
                    type = CMD_ARG;
                else
                    type = CMD_NOARG;
            }
        }
        nextCmd++;
    }
    return type;
}  // end NormApp::CommandType()


bool NormApp::ProcessCommands(int argc, const char*const* argv)
{
    int i = 1;
    while ( i < argc)
    {
        CmdType cmdType = CommandType(argv[i]);   
        switch (cmdType)
        {
            case CMD_INVALID:
                DMSG(0, "NormApp::ProcessCommands() Invalid command:%s\n", 
                        argv[i]);
                return false;
            case CMD_NOARG:
                if (!ProcessCommand(argv[i], NULL))
                {
                    DMSG(0, "NormApp::ProcessCommands() ProcessCommand(%s) error\n", 
                            argv[i]);
                    return false;
                }
                i++;
                break;
            case CMD_ARG:
                if (!ProcessCommand(argv[i], argv[i+1]))
                {
                    DMSG(0, "NormApp::ProcessCommands() ProcessCommand(%s, %s) error\n", 
                            argv[i], argv[i+1]);
                    return false;
                }
                i += 2;
                break;
        }
    }
    return true;
}  // end NormApp::ProcessCommands()

void NormApp::DoInputReady(ProtoDispatcher::Descriptor /*descriptor*/, 
                           ProtoDispatcher::Event      /*theEvent*/, 
                           const void*                 userData)
{
    ((NormApp*)userData)->OnInputReady();
}  // end NormApp::DoDataReady()


void NormApp::OnInputReady()
{
    
    //TRACE("NormApp::OnInputReady() ...\n");
    bool flush = false;
    // write to the stream while input is available _and_
    // the stream has buffer space for input
    while (input)
    {
        bool inputStarved = false;
            
        if (0 == input_length)
        {
            // We need input ...
            UINT16 readLength;
            if (input_messaging)
            {
                if (input_msg_length)
                {
                    UINT16 bufferSpace = 1024 - input_index;
                    UINT16 msgRemainder = input_msg_length - input_msg_index;
                    readLength = MIN(bufferSpace, msgRemainder);
                }
                else if (input_index < 2)
                {
                    readLength = 2 - input_index;   
                }
                else
                {
                    input_msg_length = ntohs(*((UINT16*)input_buffer));  
                    ASSERT(input_msg_length >= 2);
                    UINT16 bufferSpace = 1024 - input_index;
                    UINT16 msgRemainder = input_msg_length - 2;
                    readLength = MIN(bufferSpace, msgRemainder);  
                }
            }
            else
            {
                readLength = 1024;   
            }            
             
            // Read from "input" into our "input_buffer"
            size_t result = fread(input_buffer+input_index, sizeof(char), readLength, input);
            if (result > 0)
            {
                if (input_messaging)
                {
                    if (input_msg_length)
                    {
                        input_length = input_index + result;
                        input_index = 0;
                    }
                    else
                    {
                        input_length = 0;
                        input_index += result;
                    }
                }
                else
                {
                    input_length = input_index + result;
                    input_index = 0;    
                }             
            }
            else
            {
                if (feof(input))
                {
                    DMSG(0, "norm: input end-of-file.\n");
                    if (input_active) 
                    {
#ifdef WIN32
                        ASSERT(0);  // no Win32 support for stream i/o yet
#else
                        dispatcher.RemoveGenericInput(fileno(input));
#endif // if/else WIN32/UNIX
                        input_active = false;
                    }
                    if (stdin != input) fclose(input);
                    input = NULL;
                    flush = true;   
                }
                else if (ferror(input))
                {
                    switch (errno)
                    {
                        case EINTR:
                            continue;
                        case EAGAIN:
                            // Input not ready, will get notification when ready
                            inputStarved = true;
                            break;
                        default:
                            DMSG(0, "norm: input error:%s\n", strerror(errno));
                            break;   
                    }
                    clearerr(input);
                }
            } 
        }  // end if (0 == input_length)
        
        unsigned int writeLength = input_length;// ? input_length - input_index : 0;
            
        if (writeLength || flush)
        {
            unsigned int wroteLength = tx_stream->Write(input_buffer+input_index, 
                                                        writeLength, flush, false, 
                                                        push_stream);
            input_length -= wroteLength;
            if (0 == input_length)
                input_index = 0;
            else
                input_index += wroteLength;
            if (input_messaging) 
            {
                input_msg_index += wroteLength;
                if (input_msg_index == input_msg_length)
                {
                    input_msg_index = 0;
                    input_msg_length = 0;  
                    // Mark EOM
                    tx_stream->Write(NULL, 0, false, true, false); 
                }
            }
            if (wroteLength < writeLength) 
            {
                // Stream buffer full, temporarily deactive "input" and 
                // wait for next TX_QUEUE_EMPTY notification
                if (input_active)
                {
#ifdef WIN32
                        ASSERT(0);  // no Win32 support for stream i/o yet
#else
                        dispatcher.RemoveGenericInput(fileno(input));
#endif // if/else WIN32/UNIX
                    input_active = false;   
                }
                break;  
            }
        }
        else if (inputStarved)
        {
            // Input not ready, wait for "input" to be ready
            // (Activate/reactivate "input" as necessary
            if (!input_active)
            {
#ifdef WIN32
                ASSERT(0);  // no Win32 support for stream i/o yet
                if (false)
#else
                if (dispatcher.InstallGenericInput(fileno(input), NormApp::DoInputReady, this))
#endif // if/else WIN32/UNIX
                    input_active = true;
                else
                    DMSG(0, "NormApp::Notify(TX_QUEUE_EMPTY) error adding input notification!\n");
            } 
            break; 
        }
    }  // end while (input)
}  // NormApp::OnInputReady()


void NormApp::Notify(NormController::Event event,
                     class NormSessionMgr* sessionMgr,
                     class NormSession*    session,
                     class NormServerNode* server,
                     class NormObject*     object)
{
    switch (event)
    {
        case TX_QUEUE_EMPTY:
           // Write to stream as needed
            //DMSG(0, "NormApp::Notify(TX_QUEUE_EMPTY) ...\n");
            if (object && (object == tx_stream))
            {
                if (input) OnInputReady();
            }
            else
            {
                // Can queue a new object for transmission  
                if (interval_timer.IsActive()) interval_timer.Deactivate();
                if (interval_timer.GetInterval() > 0.0)
                    ActivateTimer(interval_timer); 
                else
                    OnIntervalTimeout(interval_timer);
            }
            break;
           
        case RX_OBJECT_NEW:
        {
            //TRACE("NormApp::Notify(RX_OBJECT_NEW) ...\n");
            // It's up to the app to "accept" the object
            switch (object->GetType())
            {
                case NormObject::STREAM:
                {
                    // Only have one rx_stream at a time for now.
                    // Reset stream i/o mgmt                    
                    output_msg_length = output_index = 0;
                    if (output_messaging) output_msg_sync = false;
                    
                    // object Size() has recommended buffering size
                    NormObjectSize size;
                    if (silent_client)
                        size = NormObjectSize((UINT32)rx_buffer_size);
                    else
                        size = object->Size();
                    
                    if (((NormStreamObject*)object)->Accept(size.LSB()))
                    {
                        rx_stream = (NormStreamObject*)object;
                    }
                    else
                    {
                        DMSG(0, "NormApp::Notify(RX_OBJECT_NEW) stream object accept error!\n");
                    }
                }
                break;                        
                case NormObject::FILE:
                {
                    if (rx_cache_path)
                    {
                        // (TBD) re-arrange so if we've already recv'd INFO,
                        // we can use that for file name right away ???
                        // (TBD) Manage recv file name collisions, etc ...
                        char fileName[PATH_MAX];
                        strcpy(fileName, rx_cache_path);
                        strcat(fileName, "normTempXXXXXX");
#ifdef WIN32
                        if (!_mktemp(fileName))
#else
                        int fd = mkstemp(fileName); 
                        if (fd >= 0)
                        {
                            close(fd);
                        }
                        else   
#endif // if/else WIN32         
                        {
                            DMSG(0, "NormApp::Notify(RX_OBJECT_NEW) Warning: mkstemp() error: %s\n",
                                    strerror(errno));  
                        } 
                        if (!((NormFileObject*)object)->Accept(fileName))
                        {
                            DMSG(0, "NormApp::Notify(RX_OBJECT_NEW) file object accept error!\n");
                        }
                    }
                    else
                    {
                        DMSG(0, "NormApp::Notify(RX_OBJECT_NEW) no rx cache for file\n");   
                    }                    
                }
                break;
                case NormObject::DATA: 
                    DMSG(0, "NormApp::Notify() FILE/DATA objects not _yet_ supported...\n");      
                    break;
            }   
            break;
        }
            
        case RX_OBJECT_INFO:
            //TRACE("NormApp::Notify(RX_OBJECT_INFO) ...\n");
            switch(object->GetType())
            {
                case NormObject::FILE:
                {
                    // Rename rx file using newly received info
                    char fileName[PATH_MAX];
                    strncpy(fileName, rx_cache_path, PATH_MAX);
                    UINT16 pathLen = strlen(rx_cache_path);
                    pathLen = MIN(pathLen, PATH_MAX);
                    UINT16 len = object->InfoLength();
                    len = MIN(len, (PATH_MAX - pathLen));
                    strncat(fileName, object->GetInfo(), len);
                    // Convert '/' in file info to directory delimiters
                    for (UINT16 i = pathLen; i < (pathLen+len); i++)
                    {
                        if ('/' == fileName[i]) 
                            fileName[i] = PROTO_PATH_DELIMITER;
                    }
                    pathLen += len;
                    if (pathLen < PATH_MAX) fileName[pathLen] = '\0';
                    
                    // Deal with concurrent rx name collisions
                    // (TBD) and implement overwrite policy
                    //       and cache files in cache mode
                    
                    if (!((NormFileObject*)object)->Rename(fileName))
                    {
                        DMSG(0, "NormApp::Notify() Error renaming rx file: %s\n",
                                fileName);
                    }
                    break;
                }
                case NormObject::DATA:
                case NormObject::STREAM:
                    break;
            }  // end switch(object->GetType())
            break;
            
        case RX_OBJECT_UPDATE:
            //TRACE("NormApp::Notify(RX_OBJECT_UPDATE) ...\n");
            switch (object->GetType())
            {
                case NormObject::FILE:
                    // (TBD) update reception progress display when applicable
                    break;
                
                case NormObject::STREAM:
                {
                    if (object != rx_stream)
                    {
                        DMSG(0, "NormApp::Notify(RX_OBJECT_UPDATE) update for invalid stream\n");
                        break;   
                    }
                    // Read the stream when it's updated  
                    ASSERT(output);
                    bool reading = true;
                    bool findMsgSync;
                    while (reading)
                    {
                        unsigned int readLength;
                        if (output_messaging)
                        {
                            if (output_msg_length)
                            {
                                readLength = output_msg_length - output_index;
                            }
                            else if (output_index < 2)
                            {
                                readLength = 2 - output_index;
                            }
                            else
                            {
                                output_msg_length = ntohs(*((UINT16*)output_buffer));
                                ASSERT(output_msg_length >= 2);
                                readLength = output_msg_length - output_index;
                            }     
                            findMsgSync = output_msg_sync ? false : true;                                            
                        }
                        else
                        {
                            output_index = 0;
                            readLength = 512; 
                            findMsgSync = false;   
                        } 
                        
                        
                        if(!((NormStreamObject*)object)->Read(output_buffer+output_index, 
                                                             &readLength, findMsgSync))
                        {
                            // The stream broke
                            if (output_messaging)
                            {
                                if (output_msg_sync)
                                    DMSG(0, "NormApp::Notify() detected broken stream ...\n");
                                output_msg_length = output_index = 0;
                                output_msg_sync = false;
                                continue;
                            }
                        }
                        else
                        {
                            if (readLength > 0)
                                output_msg_sync = true;   
                        }
                        
                        if (readLength)
                            output_index += readLength;
                        else
                            reading = false;

                        unsigned int writeLength;
                        if (output_messaging)
                        {
                            if (output_msg_length && (output_index >= output_msg_length))
                            {
                                writeLength = output_msg_length;
                                output_msg_length = 0;
                                output_index = 0;
                            }
                            else
                            {
                                writeLength = 0;
                            }
                        }
                        else
                        {
                            writeLength = readLength;
                            output_index = 0;
                        }      
                        
                        unsigned int put = 0;
                        while (put < writeLength)
                        {
                            size_t result = fwrite(output_buffer+put, sizeof(char), writeLength-put, output);
                            if (result)
                            {
                                put += result;   
                            }
                            else
                            {
                                if (ferror(output))
                                {
                                    if (EINTR == errno) 
                                    {
                                        clearerr(output);
                                        continue;
                                    }
                                    else
                                    {
                                        DMSG(0, "norm: output error:%s\n", strerror(errno));
                                        clearerr(output);
                                        break;
                                    }   
                                }
                            }   
                        }  // end while(put < nBytes)
                        if (writeLength) memset(output_buffer, 0, writeLength); 
                    }  // end while (reading)
                    fflush(output);
                    break;
                }
                                        
                case NormObject::DATA: 
                    DMSG(0, "NormApp::Notify() DATA objects not _yet_ supported...\n");      
                    break;
            }  // end switch (object->GetType())
            break;
            
        case RX_OBJECT_COMPLETE:
        {
            //TRACE("NormApp::Notify(RX_OBJECT_COMPLETE) ...\n");
            switch(object->GetType())
            {
                case NormObject::FILE:
                {
                    const char* filePath = ((NormFileObject*)object)->Path();
                    //DMSG(0, "norm: Completed rx file: %s\n", filePath);
                    if (post_processor->IsEnabled())
                    {
                        if (!post_processor->ProcessFile(filePath))
                        {
                            DMSG(0, "norm: post processing error\n");
                        }  
                    }
                    break;
                }
                    
                case NormObject::STREAM:
                    ASSERT(0);
                    break;
                case NormObject::DATA:
                    ASSERT(0);
                    break;
            }
            break;
        }
    }  // end switch(event)
}  // end NormApp::Notify()


bool NormApp::OnIntervalTimeout(ProtoTimer& /*theTimer*/)
{
    char fileName[PATH_MAX];
    if (tx_file_list.GetNextFile(fileName))
    {
        char pathName[PATH_MAX];
        tx_file_list.GetCurrentBasePath(pathName);
        unsigned int len = strlen(pathName);
        len = MIN(len, PATH_MAX);
        unsigned int maxLen = PATH_MAX - len;
        char* ptr = fileName + len;
        len = strlen(ptr);
        len = MIN(len, maxLen);
        // (TBD) Make sure len <= segment_size)
        char fileNameInfo[PATH_MAX];
        strncpy(fileNameInfo, ptr, len);
        // Normalize directory delimiters in file name info
        for (unsigned int i = 0; i < len; i++)
        {
            if (PROTO_PATH_DELIMITER == fileNameInfo[i]) 
                fileNameInfo[i] = '/';
        }
        char temp[PATH_MAX];
        strncpy(temp, fileNameInfo, len);
        temp[len] = '\0';
        if (!session->QueueTxFile(fileName, fileNameInfo, len))
        {
            DMSG(0, "NormApp::OnIntervalTimeout() Error queuing tx file: %s\n",
                    fileName);
            // Wait a second, then try the next file in the list
            if (interval_timer.IsActive()) interval_timer.Deactivate();
            interval_timer.SetInterval(1.0);
            ActivateTimer(interval_timer);
            return false;
        }
        //DMSG(0, "norm: File \"%s\" queued for transmission.\n", fileName);
        interval_timer.SetInterval(tx_object_interval);
    }
    else if (tx_repeat_count)
    {
        // (TBD) When repeating, remove previous instance from tx queue???
        if (tx_repeat_count > 0) tx_repeat_count--;
        tx_file_list.ResetIterator();
        if (tx_repeat_interval > tx_object_interval)
        {
            if (interval_timer.IsActive()) interval_timer.Deactivate();
            interval_timer.SetInterval(tx_repeat_interval = tx_object_interval);
            ActivateTimer(interval_timer);
            return false;       
        }
        else
        {
            return OnIntervalTimeout(interval_timer);
        }
    }
    else
    {
        DMSG(0, "norm: End of tx file list reached.\n");  
    }   
    return true;
}  // end NormApp::OnIntervalTimeout()

bool NormApp::OnStartup(int argc, const char*const* argv)
{
    
    if (!(post_processor = NormPostProcessor::Create()))
    {
        DMSG(0, "NormApp::OnStartup() error creating post processor\n");
        return false;   
    }
    
    if (!ProcessCommands(argc, argv))
    {
        DMSG(0, "NormApp::OnStartup() error processing command-line commands\n");
        return false; 
    }

#ifdef UNIX    
    signal(SIGCHLD, SignalHandler);
#endif // UNIX
        
    // Validate our application settings
    if (!address)
    {
        DMSG(0, "NormApp::OnStartup() Error! no session address given.\n");
        return false;
    }
    if (!input && !output && tx_file_list.IsEmpty() && !rx_cache_path)
    {
        DMSG(0, "NormApp::OnStartup() Error! no \"input\", \"output\", "
                "\"sendfile\", or \"rxcache\" given.\n");
        return false;
    }   
    
    // Create a new session on multicast group/port
    session = session_mgr.NewSession(address, port);
    if (session)
    {
        // Common session parameters
        session->SetTxRate(tx_rate);
        session->SetTrace(tracing);
        session->SetTxLoss(tx_loss);
        session->SetRxLoss(rx_loss);
        session->SetBackoffFactor(backoff_factor);          
            
        if (input || !tx_file_list.IsEmpty())
        {
            NormObjectId baseId = (unsigned short)(rand() * (65535.0/ (double)RAND_MAX));
            session->ServerSetBaseObjectId(baseId);
            
            session->SetCongestionControl(cc_enable);
            if (!session->StartServer(tx_buffer_size, segment_size, ndata, nparity))
            {
                DMSG(0, "NormApp::OnStartup() start server error!\n");
                session_mgr.Destroy();
                return false;
            }
            session->ServerSetAutoParity(auto_parity);
            session->ServerSetExtraParity(extra_parity);              
            if (input)
            {
                // Open a stream object to write to (QueueTxStream(stream bufferSize))
                tx_stream = session->QueueTxStream(tx_buffer_size);
                if (!tx_stream)
                {
                    DMSG(0, "NormApp::OnStartup() queue tx stream error!\n");
                    session_mgr.Destroy();
                    return false;
                }
            }
        }
        
        if (output || rx_cache_path)
        {
            // StartClient(bufferMax (per-sender))
            session->SetUnicastNacks(unicast_nacks);
            session->ClientSetSilent(silent_client);
            if (!session->StartClient(rx_buffer_size))
            {
                DMSG(0, "NormApp::OnStartup() start client error!\n");
                session_mgr.Destroy();
                return false;
            }
        }
        return true;
    }
    else
    {
        DMSG(0, "NormApp::OnStartup() new session error!\n");
        return false;
    }
}  // end NormApp::OnStartup()

void NormApp::OnShutdown()
{
    //TRACE("NormApp::OnShutdown() ...\n");
    session_mgr.Destroy();
    if (input)
    {
        if (input_active) 
        {
#ifdef WIN32
            ASSERT(0);   // no Win32 support for stream i/o yet
#else
            dispatcher.RemoveGenericInput(fileno(input));
#endif // if/else WIN32/UNIX
            input_active = false;
        }
        if (stdin != input) fclose(input);
        input = NULL;
    }
    if (output) 
    {
        if (stdout != output) fclose(output);
        output = NULL;
    }
    if (post_processor)
    {
        delete post_processor;
        post_processor = NULL;   
    }
}  // end NormApp::OnShutdown()

// Out application instance 
PROTO_INSTANTIATE_APP(NormApp);

#ifdef UNIX
void NormApp::SignalHandler(int sigNum)
{
    switch(sigNum)
    {
        case SIGCHLD:
        {
            NormApp* app = static_cast<NormApp*>(ProtoApp::GetApp());
            if (app->post_processor) app->post_processor->OnDeath();
            signal(SIGCHLD, SignalHandler);
            break;
        }
            
        default:
            fprintf(stderr, "norm: Unexpected signal: %d\n", sigNum);
            break; 
    }  
}  // end NormApp::SignalHandler()
#endif // UNIX