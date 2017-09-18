/* Copyright (C) 2007-2017 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Pablo Rincon Crespo <pablo.rincon.crespo@gmail.com>
 * \author Eric Leblond <eric@regit.org>
 *
 * App Layer Parser for FTP
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "threads.h"

#include "util-print.h"
#include "util-pool.h"

#include "flow-util.h"
#include "flow-storage.h"

#include "detect-engine-state.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"

#include "app-layer.h"
#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-ftp.h"
#include "app-layer-expectation.h"

#include "util-spm.h"
#include "util-unittest.h"
#include "util-debug.h"
#include "util-memcmp.h"

#ifdef HAVE_RUST
#include "rust-ftp-mod-gen.h"
#endif

static int FTPGetLineForDirection(FtpState *state, FtpLineState *line_state)
{
    void *ptmp;
    if (line_state->current_line_lf_seen == 1) {
        /* we have seen the lf for the previous line.  Clear the parser
         * details to parse new line */
        line_state->current_line_lf_seen = 0;
        if (line_state->current_line_db == 1) {
            line_state->current_line_db = 0;
            SCFree(line_state->db);
            line_state->db = NULL;
            line_state->db_len = 0;
            state->current_line = NULL;
            state->current_line_len = 0;
        }
    }

    uint8_t *lf_idx = memchr(state->input, 0x0a, state->input_len);

    if (lf_idx == NULL) {
        /* fragmented lines.  Decoder event for special cases.  Not all
         * fragmented lines should be treated as a possible evasion
         * attempt.  With multi payload ftp chunks we can have valid
         * cases of fragmentation.  But within the same segment chunk
         * if we see fragmentation then it's definitely something you
         * should alert about */
        if (line_state->current_line_db == 0) {
            line_state->db = SCMalloc(state->input_len);
            if (line_state->db == NULL) {
                return -1;
            }
            line_state->current_line_db = 1;
            memcpy(line_state->db, state->input, state->input_len);
            line_state->db_len = state->input_len;
        } else {
            ptmp = SCRealloc(line_state->db,
                             (line_state->db_len + state->input_len));
            if (ptmp == NULL) {
                SCFree(line_state->db);
                line_state->db = NULL;
                line_state->db_len = 0;
                return -1;
            }
            line_state->db = ptmp;

            memcpy(line_state->db + line_state->db_len,
                   state->input, state->input_len);
            line_state->db_len += state->input_len;
        }
        state->input += state->input_len;
        state->input_len = 0;

        return -1;

    } else {
        line_state->current_line_lf_seen = 1;

        if (line_state->current_line_db == 1) {
            ptmp = SCRealloc(line_state->db,
                             (line_state->db_len + (lf_idx + 1 - state->input)));
            if (ptmp == NULL) {
                SCFree(line_state->db);
                line_state->db = NULL;
                line_state->db_len = 0;
                return -1;
            }
            line_state->db = ptmp;

            memcpy(line_state->db + line_state->db_len,
                   state->input, (lf_idx + 1 - state->input));
            line_state->db_len += (lf_idx + 1 - state->input);

            if (line_state->db_len > 1 &&
                line_state->db[line_state->db_len - 2] == 0x0D) {
                line_state->db_len -= 2;
                state->current_line_delimiter_len = 2;
            } else {
                line_state->db_len -= 1;
                state->current_line_delimiter_len = 1;
            }

            state->current_line = line_state->db;
            state->current_line_len = line_state->db_len;

        } else {
            state->current_line = state->input;
            state->current_line_len = lf_idx - state->input;

            if (state->input != lf_idx &&
                *(lf_idx - 1) == 0x0D) {
                state->current_line_len--;
                state->current_line_delimiter_len = 2;
            } else {
                state->current_line_delimiter_len = 1;
            }
        }

        state->input_len -= (lf_idx - state->input) + 1;
        state->input = (lf_idx + 1);

        return 0;
    }

}

static int FTPGetLine(FtpState *state)
{
    SCEnter();

    /* we have run out of input */
    if (state->input_len <= 0)
        return -1;

    /* toserver */
    if (state->direction == 0)
        return FTPGetLineForDirection(state, &state->line_state[0]);
    else
        return FTPGetLineForDirection(state, &state->line_state[1]);
}

/**
 * \brief This function is called to determine and set which command is being
 * transfered to the ftp server
 * \param ftp_state the ftp state structure for the parser
 * \param input input line of the command
 * \param len of the command
 *
 * \retval 1 when the command is parsed, 0 otherwise
 */
static int FTPParseRequestCommand(void *ftp_state, uint8_t *input,
                                  uint32_t input_len)
{
    SCEnter();
    FtpState *fstate = (FtpState *)ftp_state;
    fstate->command = FTP_COMMAND_UNKNOWN;

    if (input_len >= 4 && SCMemcmpLowercase("port", input, 4) == 0) {
        fstate->command = FTP_COMMAND_PORT;
    }

    if (input_len >= 8 && SCMemcmpLowercase("auth tls", input, 8) == 0) {
        fstate->command = FTP_COMMAND_AUTH_TLS;
    }

    if (input_len >= 4 && SCMemcmpLowercase("pasv", input, 4) == 0) {
        fstate->command = FTP_COMMAND_PASV;
    }

    if (input_len >= 4 && SCMemcmpLowercase("retr", input, 4) == 0) {
        fstate->command = FTP_COMMAND_RETR;
    }

    if (input_len >= 4 && SCMemcmpLowercase("epsv", input, 4) == 0) {
        fstate->command = FTP_COMMAND_EPSV;
    }

    if (input_len >= 4 && SCMemcmpLowercase("stor", input, 4) == 0) {
        fstate->command = FTP_COMMAND_STOR;
    }

    return 1;
}

/**
 * \brief This function is called to retrieve a ftp request
 * \param ftp_state the ftp state structure for the parser
 * \param input input line of the command
 * \param input_len length of the request
 * \param output the resulting output
 *
 * \retval 1 when the command is parsed, 0 otherwise
 */
static int FTPParseRequest(Flow *f, void *ftp_state,
                           AppLayerParserState *pstate,
                           uint8_t *input, uint32_t input_len,
                           void *local_data)
{
    SCEnter();
    /* PrintRawDataFp(stdout, input,input_len); */

    FtpState *state = (FtpState *)ftp_state;
    void *ptmp;

    if (input == NULL && AppLayerParserStateIssetFlag(pstate, APP_LAYER_PARSER_EOF)) {
        SCReturnInt(1);
    } else if (input == NULL || input_len == 0) {
        SCReturnInt(-1);
    }

    state->input = input;
    state->input_len = input_len;
    /* toserver stream */
    state->direction = 0;

    while (FTPGetLine(state) >= 0) {
        FTPParseRequestCommand(state,
                               state->current_line, state->current_line_len);
        switch (state->command) {
            case FTP_COMMAND_PORT:
                if (state->current_line_len > state->port_line_size) {
                    ptmp = SCRealloc(state->port_line, state->current_line_len);
                    if (ptmp == NULL) {
                        SCFree(state->port_line);
                        state->port_line = NULL;
                        state->port_line_size = 0;
                        return 0;
                    }
                    state->port_line = ptmp;

                    state->port_line_size = state->current_line_len;
                }
                memcpy(state->port_line, state->current_line,
                        state->current_line_len);
                state->port_line_len = state->current_line_len;
                break;
            case FTP_COMMAND_RETR:
            case FTP_COMMAND_STOR:
                {
                    char *data = SCCalloc(state->current_line_len + sizeof(int64_t) + 1, sizeof(*data));
                    if (data == NULL)
                        SCReturnInt(-1);
                    memcpy(data + sizeof(int64_t), state->current_line, state->current_line_len);
                    *(int64_t *) data = FlowGetId(f);
                    int ret  = AppLayerExpectationCreate(f, STREAM_TOSERVER, 0,
                            state->dyn_port, ALPROTO_FTPDATA, data);
                    if (ret == -1) {
                        SCLogError(SC_ERR_INVALID_VALUE, "No expectation created.");
                    } else {
                        SCLogDebug("Expectation created.");
                    }
                }
                break;
            default:
                break;
        }
    }

    return 1;
}

static int FTPParsePassiveResponse(Flow *f, void *ftp_state, uint8_t *input, uint32_t input_len)
{
    FtpState *state = (FtpState *)ftp_state;
    uint16_t dyn_port;

#ifdef HAVE_RUST
    dyn_port = rs_ftp_pasv_response(input, input_len);
    if (dyn_port == 0) {
        return -1;
    }
#else
    uint16_t part1, part2;
    uint8_t *ptr;
    ptr = memrchr(input, ',', input_len);
    if (ptr == NULL)
        return -1;

    part2 = atoi((char *)ptr + 1);
    ptr = memrchr(input, ',', (ptr - input) - 1);
    if (ptr == NULL)
        return -1;
    part1 = atoi((char *)ptr + 1);

    dyn_port = 256 * part1 + part2;
#endif
    state->dyn_port = dyn_port;


    return 0;
}

static int FTPParsePassiveResponseV6(Flow *f, void *ftp_state, uint8_t *input, uint32_t input_len)
{
    FtpState *state = (FtpState *)ftp_state;

#ifdef HAVE_RUST
    uint16_t dyn_port = rs_ftp_epsv_response(input, input_len);
    if (dyn_port == 0) {
        return -1;
    }

    state->dyn_port = dyn_port;
#else
    uint8_t *ptr;

    ptr = memrchr(input, '|', input_len);
    if (ptr == NULL) {
        return -1;
    } else {
        int n_length =  ptr - input - 1;
        if (n_length < 4)
            return -1;
        ptr = memrchr(input, '|', n_length);
        if (ptr == NULL)
            return -1;
    }
    state->dyn_port = atoi((char *)ptr + 1);
#endif
    return 0;
}

/**
 * \brief This function is called to retrieve a ftp response
 * \param ftp_state the ftp state structure for the parser
 * \param input input line of the command
 * \param input_len length of the request
 * \param output the resulting output
 *
 * \retval 1 when the command is parsed, 0 otherwise
 */
static int FTPParseResponse(Flow *f, void *ftp_state, AppLayerParserState *pstate,
                            uint8_t *input, uint32_t input_len,
                            void *local_data)
{
    FtpState *state = (FtpState *)ftp_state;

    if (state->command == FTP_COMMAND_AUTH_TLS) {
        if (input_len >= 4 && SCMemcmp("234 ", input, 4) == 0) {
            AppLayerRequestProtocolTLSUpgrade(f);
        }
    }

    if (state->command == FTP_COMMAND_PASV) {
        if (input_len >= 4 && SCMemcmp("227 ", input, 4) == 0) {
            FTPParsePassiveResponse(f, ftp_state, input, input_len);
        }
    }

    if (state->command == FTP_COMMAND_EPSV) {
        if (input_len >= 4 && SCMemcmp("229 ", input, 4) == 0) {
            FTPParsePassiveResponseV6(f, ftp_state, input, input_len);
        }
    }

    return 1;
}

#ifdef DEBUG
static SCMutex ftp_state_mem_lock = SCMUTEX_INITIALIZER;
static uint64_t ftp_state_memuse = 0;
static uint64_t ftp_state_memcnt = 0;
#endif

static void *FTPStateAlloc(void)
{
    void *s = SCMalloc(sizeof(FtpState));
    if (unlikely(s == NULL))
        return NULL;

    memset(s, 0, sizeof(FtpState));

#ifdef DEBUG
    SCMutexLock(&ftp_state_mem_lock);
    ftp_state_memcnt++;
    ftp_state_memuse+=sizeof(FtpState);
    SCMutexUnlock(&ftp_state_mem_lock);
#endif
    return s;
}

static void FTPStateFree(void *s)
{
    FtpState *fstate = (FtpState *) s;
    if (fstate->port_line != NULL)
        SCFree(fstate->port_line);
    if (fstate->line_state[0].db)
        SCFree(fstate->line_state[0].db);
    if (fstate->line_state[1].db)
        SCFree(fstate->line_state[1].db);

    //AppLayerDecoderEventsFreeEvents(&s->decoder_events);

    if (fstate->de_state != NULL) {
        DetectEngineStateFree(fstate->de_state);
    }

    SCFree(s);
#ifdef DEBUG
    SCMutexLock(&ftp_state_mem_lock);
    ftp_state_memcnt--;
    ftp_state_memuse-=sizeof(FtpState);
    SCMutexUnlock(&ftp_state_mem_lock);
#endif
}

static int FTPStateHasTxDetectState(void *state)
{
    FtpState *ftp_state = (FtpState *)state;
    if (ftp_state->de_state)
        return 1;
    return 0;
}

static int FTPSetTxDetectState(void *state, void *vtx, DetectEngineState *de_state)
{
    FtpState *ftp_state = (FtpState *)state;
    ftp_state->de_state = de_state;
    return 0;
}

static DetectEngineState *FTPGetTxDetectState(void *vtx)
{
    FtpState *ftp_state = (FtpState *)vtx;
    return ftp_state->de_state;
}

static void FTPStateTransactionFree(void *state, uint64_t tx_id)
{
    /* do nothing */
}

static void *FTPGetTx(void *state, uint64_t tx_id)
{
    FtpState *ftp_state = (FtpState *)state;
    return ftp_state;
}

static uint64_t FTPGetTxCnt(void *state)
{
    /* single tx */
    return 1;
}

static int FTPGetAlstateProgressCompletionStatus(uint8_t direction)
{
    return FTP_STATE_FINISHED;
}

static int FTPGetAlstateProgress(void *tx, uint8_t direction)
{
    FtpState *ftp_state = (FtpState *)tx;

    if (direction == STREAM_TOSERVER &&
        ftp_state->command == FTP_COMMAND_PORT) {
        return FTP_STATE_PORT_DONE;
    }

    /* TODO: figure out further progress handling */

    return FTP_STATE_IN_PROGRESS;
}


static int FTPRegisterPatternsForProtocolDetection(void)
{
    if (AppLayerProtoDetectPMRegisterPatternCI(IPPROTO_TCP, ALPROTO_FTP,
                                              "220 (", 5, 0, STREAM_TOCLIENT) < 0)
    {
        return -1;
    }
    if (AppLayerProtoDetectPMRegisterPatternCI(IPPROTO_TCP, ALPROTO_FTP,
                                               "FEAT", 4, 0, STREAM_TOSERVER) < 0)
    {
        return -1;
    }
    if (AppLayerProtoDetectPMRegisterPatternCI(IPPROTO_TCP, ALPROTO_FTP,
                                               "USER ", 5, 0, STREAM_TOSERVER) < 0)
    {
        return -1;
    }
    if (AppLayerProtoDetectPMRegisterPatternCI(IPPROTO_TCP, ALPROTO_FTP,
                                               "PASS ", 5, 0, STREAM_TOSERVER) < 0)
    {
        return -1;
    }
    if (AppLayerProtoDetectPMRegisterPatternCI(IPPROTO_TCP, ALPROTO_FTP,
                                               "PORT ", 5, 0, STREAM_TOSERVER) < 0)
    {
        return -1;
    }

    return 0;
}


static StreamingBufferConfig sbcfg = STREAMING_BUFFER_CONFIG_INITIALIZER;

/**
 * \brief This function is called to retrieve a ftp request
 * \param ftp_state the ftp state structure for the parser
 * \param input input line of the command
 * \param input_len length of the request
 * \param output the resulting output
 *
 * \retval 1 when the command is parsed, 0 otherwise
 */
static int FTPDataParse(Flow *f, void *ftp_state,
        AppLayerParserState *pstate,
        uint8_t *input, uint32_t input_len,
        void *local_data, int direction)
{
    FtpDataState *ftpdata_state = (FtpDataState *)ftp_state;
    uint16_t flags = FileFlowToFlags(f, direction);
    int ret = 0;
    /* we depend on detection engine for file pruning */
    flags |= FILE_USE_DETECT;
    if (ftpdata_state->files == NULL) {
        const char *data = (char *)FlowGetStorageById(f, AppLayerExpectationGetDataId());
        const char *filename;
        if (data == NULL) {
            SCReturnInt(-1);
        }

        ftpdata_state->files = FileContainerAlloc();
        if (ftpdata_state->files == NULL) {
            SCLogError(SC_ERR_MEM_ALLOC, "Could not create file container");
            SCReturnInt(-1);
        }

        filename = data + sizeof(int64_t) + 5;
        ftpdata_state->filename = SCStrdup(filename);
        if (ftpdata_state->filename == NULL) {
            SCLogInfo("Unable to allocate filename");
        }
        f->parent_id = *(int64_t *)data;
        if (input_len >= 4 && SCMemcmpLowercase("retr", data + sizeof(int64_t), 4) == 0) {
            ftpdata_state->command = FTP_COMMAND_RETR;
        }
        if (input_len >= 4 && SCMemcmpLowercase("stor", data + sizeof(int64_t), 4) == 0) {
            ftpdata_state->command = FTP_COMMAND_STOR;
        }

        if (FileOpenFile(ftpdata_state->files, &sbcfg, (uint8_t *) filename, strlen(filename),
                         input, input_len, flags) == NULL) {
            SCLogError(SC_ERR_FOPEN, "Can't open file");
            ret = -1;
        }
        FlowFreeStorageById(f, AppLayerExpectationGetDataId());
    } else {
        if (input_len != 0) {
            ret = FileAppendData(ftpdata_state->files, input, input_len);
            if (ret == -2) {
                ret = 0;
                SCLogDebug("FileAppendData() - file no longer being extracted");
            } else if (ret < 0) {
                SCLogDebug("FileAppendData() failed: %d", ret);
                ret = -2;
            }
        } else {
            ret = FileCloseFile(ftpdata_state->files, NULL, 0, flags);
        }
    }

    if (AppLayerParserStateIssetFlag(pstate, APP_LAYER_PARSER_EOF)) {
        ret = FileCloseFile(ftpdata_state->files, (uint8_t *) NULL, 0, flags);
    }
    if (ftpdata_state->files) {
        FilePrune(ftpdata_state->files);
    }
    return ret;
}

static int FTPDataParseRequest(Flow *f, void *ftp_state,
        AppLayerParserState *pstate,
        uint8_t *input, uint32_t input_len,
        void *local_data)
{
    return FTPDataParse(f, ftp_state, pstate, input, input_len,
                               local_data, STREAM_TOSERVER);
}

static int FTPDataParseResponse(Flow *f, void *ftp_state,
        AppLayerParserState *pstate,
        uint8_t *input, uint32_t input_len,
        void *local_data)
{
    return FTPDataParse(f, ftp_state, pstate, input, input_len,
                               local_data, STREAM_TOCLIENT);
}

#ifdef DEBUG
static SCMutex ftpdata_state_mem_lock = SCMUTEX_INITIALIZER;
static uint64_t ftpdata_state_memuse = 0;
static uint64_t ftpdata_state_memcnt = 0;
#endif

static void *FTPDataStateAlloc(void)
{
    void *s = SCMalloc(sizeof(FtpDataState));
    if (unlikely(s == NULL))
        return NULL;

    memset(s, 0, sizeof(FtpDataState));

#ifdef DEBUG
    SCMutexLock(&ftpdata_state_mem_lock);
    ftpdata_state_memcnt++;
    ftpdata_state_memuse+=sizeof(FtpDataState);
    SCMutexUnlock(&ftpdata_state_mem_lock);
#endif
    return s;
}

static void FTPDataStateFree(void *s)
{
    FtpDataState *fstate = (FtpDataState *) s;

    if (fstate->de_state != NULL) {
        DetectEngineStateFree(fstate->de_state);
    }
    if (fstate->filename != NULL) {
        SCFree(fstate->filename);
    }

    FileContainerFree(fstate->files);

    SCFree(s);
#ifdef DEBUG
    SCMutexLock(&ftpdata_state_mem_lock);
    ftpdata_state_memcnt--;
    ftpdata_state_memuse-=sizeof(FtpDataState);
    SCMutexUnlock(&ftpdata_state_mem_lock);
#endif
}

static int FTPDataStateHasTxDetectState(void *state)
{
    FtpDataState *ftp_state = (FtpDataState *)state;
    if (ftp_state->de_state)
        return 1;
    return 0;
}

static int FTPDataSetTxDetectState(void *state, void *vtx, DetectEngineState *de_state)
{
    FtpDataState *ftp_state = (FtpDataState *)state;
    ftp_state->de_state = de_state;
    return 0;
}

static DetectEngineState *FTPDataGetTxDetectState(void *vtx)
{
    FtpDataState *ftp_state = (FtpDataState *)vtx;
    return ftp_state->de_state;
}

static void FTPDataStateTransactionFree(void *state, uint64_t tx_id)
{
    /* do nothing */
}

static void *FTPDataGetTx(void *state, uint64_t tx_id)
{
    FtpDataState *ftp_state = (FtpDataState *)state;
    return ftp_state;
}

static uint64_t FTPDataGetTxCnt(void *state)
{
    /* single tx */
    return 1;
}

static int FTPDataGetAlstateProgressCompletionStatus(uint8_t direction)
{
    return FTPDATA_STATE_FINISHED;
}

static int FTPDataGetAlstateProgress(void *tx, uint8_t direction)
{
    //FtpDataState *ftpdata_state = (FtpDataState *)tx;

    /* FIXME */

    return FTPDATA_STATE_IN_PROGRESS;
}

static FileContainer *FTPDataStateGetFiles(void *state, uint8_t direction)
{
    FtpDataState *ftpdata_state = (FtpDataState *)state;

    SCReturnPtr(ftpdata_state->files, "FileContainer");
}

void RegisterFTPParsers(void)
{
    const char *proto_name = "ftp";
    const char *proto_data_name = "ftp-data";

    /** FTP */
    if (AppLayerProtoDetectConfProtoDetectionEnabled("tcp", proto_name)) {
        AppLayerProtoDetectRegisterProtocol(ALPROTO_FTP, proto_name);
        if (FTPRegisterPatternsForProtocolDetection() < 0 )
            return;
        AppLayerProtoDetectRegisterProtocol(ALPROTO_FTPDATA, proto_data_name);
    }

    if (AppLayerParserConfParserEnabled("tcp", proto_name)) {
        AppLayerParserRegisterParser(IPPROTO_TCP, ALPROTO_FTP, STREAM_TOSERVER,
                                     FTPParseRequest);
        AppLayerParserRegisterParser(IPPROTO_TCP, ALPROTO_FTP, STREAM_TOCLIENT,
                                     FTPParseResponse);
        AppLayerParserRegisterStateFuncs(IPPROTO_TCP, ALPROTO_FTP, FTPStateAlloc, FTPStateFree);
        AppLayerParserRegisterParserAcceptableDataDirection(IPPROTO_TCP, ALPROTO_FTP, STREAM_TOSERVER | STREAM_TOCLIENT);

        AppLayerParserRegisterTxFreeFunc(IPPROTO_TCP, ALPROTO_FTP, FTPStateTransactionFree);

        AppLayerParserRegisterDetectStateFuncs(IPPROTO_TCP, ALPROTO_FTP, FTPStateHasTxDetectState,
                                               FTPGetTxDetectState, FTPSetTxDetectState);

        AppLayerParserRegisterGetTx(IPPROTO_TCP, ALPROTO_FTP, FTPGetTx);

        AppLayerParserRegisterGetTxCnt(IPPROTO_TCP, ALPROTO_FTP, FTPGetTxCnt);

        AppLayerParserRegisterGetStateProgressFunc(IPPROTO_TCP, ALPROTO_FTP, FTPGetAlstateProgress);

        AppLayerParserRegisterGetStateProgressCompletionStatus(ALPROTO_FTP,
                                                               FTPGetAlstateProgressCompletionStatus);


        AppLayerRegisterExpectationProto(IPPROTO_TCP, ALPROTO_FTPDATA);
        AppLayerParserRegisterParser(IPPROTO_TCP, ALPROTO_FTPDATA, STREAM_TOSERVER,
                                     FTPDataParseRequest);
        AppLayerParserRegisterParser(IPPROTO_TCP, ALPROTO_FTPDATA, STREAM_TOCLIENT,
                                     FTPDataParseResponse);
        AppLayerParserRegisterStateFuncs(IPPROTO_TCP, ALPROTO_FTPDATA, FTPDataStateAlloc, FTPDataStateFree);
        AppLayerParserRegisterParserAcceptableDataDirection(IPPROTO_TCP, ALPROTO_FTPDATA, STREAM_TOSERVER | STREAM_TOCLIENT);
        AppLayerParserRegisterTxFreeFunc(IPPROTO_TCP, ALPROTO_FTPDATA, FTPDataStateTransactionFree);
        AppLayerParserRegisterDetectStateFuncs(IPPROTO_TCP, ALPROTO_FTPDATA, FTPDataStateHasTxDetectState,
                FTPDataGetTxDetectState, FTPDataSetTxDetectState);

        AppLayerParserRegisterGetFilesFunc(IPPROTO_TCP, ALPROTO_FTPDATA, FTPDataStateGetFiles);

        AppLayerParserRegisterGetTx(IPPROTO_TCP, ALPROTO_FTPDATA, FTPDataGetTx);

        AppLayerParserRegisterGetTxCnt(IPPROTO_TCP, ALPROTO_FTPDATA, FTPDataGetTxCnt);

        AppLayerParserRegisterGetStateProgressFunc(IPPROTO_TCP, ALPROTO_FTPDATA, FTPDataGetAlstateProgress);

        AppLayerParserRegisterGetStateProgressCompletionStatus(ALPROTO_FTPDATA,
                FTPDataGetAlstateProgressCompletionStatus);

        sbcfg.buf_size = 4096;

    } else {
        SCLogInfo("Parsed disabled for %s protocol. Protocol detection"
                  "still on.", proto_name);
    }
#ifdef UNITTESTS
    AppLayerParserRegisterProtocolUnittests(IPPROTO_TCP, ALPROTO_FTP, FTPParserRegisterTests);
#endif
}

void FTPAtExitPrintStats(void)
{
#ifdef DEBUG
    SCMutexLock(&ftp_state_mem_lock);
    SCLogDebug("ftp_state_memcnt %"PRIu64", ftp_state_memuse %"PRIu64"",
               ftp_state_memcnt, ftp_state_memuse);
    SCMutexUnlock(&ftp_state_mem_lock);
#endif
}


#ifdef HAVE_LIBJANSSON
json_t *JsonFTPDataAddMetadata(const Flow *f)
{
    FtpDataState *ftp_state = NULL;
    if (f->alstate == NULL)
        return NULL;
    ftp_state = (FtpDataState *)f->alstate;
    json_t *ftpd = json_object();
    if (ftpd == NULL)
        return NULL;
    if (ftp_state->filename) {
        json_object_set_new(ftpd, "filename", json_string(ftp_state->filename));
    }
    switch (ftp_state->command) {
        case FTP_COMMAND_STOR:
            json_object_set_new(ftpd, "command", json_string("STOR"));
            break;
        case FTP_COMMAND_RETR:
            json_object_set_new(ftpd, "command", json_string("RETR"));
            break;
        default:
            break;
    }
    return ftpd;
}
#endif /* HAVE_LIBJANSSON */

/* UNITTESTS */
#ifdef UNITTESTS

/** \test Send a get request in one chunk. */
static int FTPParserTest01(void)
{
    int result = 1;
    Flow f;
    uint8_t ftpbuf[] = "PORT 192,168,1,1,0,80\r\n";
    uint32_t ftplen = sizeof(ftpbuf) - 1; /* minus the \0 */
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.alproto = ALPROTO_FTP;

    StreamTcpInitConfig(TRUE);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_FTP,
                                STREAM_TOSERVER | STREAM_EOF, ftpbuf, ftplen);
    if (r != 0) {
        SCLogDebug("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        FLOWLOCK_UNLOCK(&f);
        goto end;
    }
    FLOWLOCK_UNLOCK(&f);

    FtpState *ftp_state = f.alstate;
    if (ftp_state == NULL) {
        SCLogDebug("no ftp state: ");
        result = 0;
        goto end;
    }

    if (ftp_state->command != FTP_COMMAND_PORT) {
        SCLogDebug("expected command %" PRIu32 ", got %" PRIu32 ": ", FTP_COMMAND_PORT, ftp_state->command);
        result = 0;
        goto end;
    }

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    return result;
}

/** \test Send a splitted get request. */
static int FTPParserTest03(void)
{
    int result = 1;
    Flow f;
    uint8_t ftpbuf1[] = "POR";
    uint32_t ftplen1 = sizeof(ftpbuf1) - 1; /* minus the \0 */
    uint8_t ftpbuf2[] = "T 192,168,1";
    uint32_t ftplen2 = sizeof(ftpbuf2) - 1; /* minus the \0 */
    uint8_t ftpbuf3[] = "1,1,10,20\r\n";
    uint32_t ftplen3 = sizeof(ftpbuf3) - 1; /* minus the \0 */
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.alproto = ALPROTO_FTP;

    StreamTcpInitConfig(TRUE);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_FTP,
                                STREAM_TOSERVER | STREAM_START, ftpbuf1,
                                ftplen1);
    if (r != 0) {
        SCLogDebug("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        FLOWLOCK_UNLOCK(&f);
        goto end;
    }
    FLOWLOCK_UNLOCK(&f);

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_FTP, STREAM_TOSERVER,
                            ftpbuf2, ftplen2);
    if (r != 0) {
        SCLogDebug("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        FLOWLOCK_UNLOCK(&f);
        goto end;
    }
    FLOWLOCK_UNLOCK(&f);

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_FTP,
                            STREAM_TOSERVER | STREAM_EOF, ftpbuf3, ftplen3);
    if (r != 0) {
        SCLogDebug("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        FLOWLOCK_UNLOCK(&f);
        goto end;
    }
    FLOWLOCK_UNLOCK(&f);

    FtpState *ftp_state = f.alstate;
    if (ftp_state == NULL) {
        SCLogDebug("no ftp state: ");
        result = 0;
        goto end;
    }

    if (ftp_state->command != FTP_COMMAND_PORT) {
        SCLogDebug("expected command %" PRIu32 ", got %" PRIu32 ": ", FTP_COMMAND_PORT, ftp_state->command);
        result = 0;
        goto end;
    }

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test See how it deals with an incomplete request. */
static int FTPParserTest06(void)
{
    int result = 1;
    Flow f;
    uint8_t ftpbuf1[] = "PORT";
    uint32_t ftplen1 = sizeof(ftpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.alproto = ALPROTO_FTP;

    StreamTcpInitConfig(TRUE);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_FTP,
                                STREAM_TOSERVER | STREAM_START | STREAM_EOF,
                                ftpbuf1,
                                ftplen1);
    if (r != 0) {
        SCLogDebug("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        FLOWLOCK_UNLOCK(&f);
        goto end;
    }
    FLOWLOCK_UNLOCK(&f);

    FtpState *ftp_state = f.alstate;
    if (ftp_state == NULL) {
        SCLogDebug("no ftp state: ");
        result = 0;
        goto end;
    }

    if (ftp_state->command != FTP_COMMAND_UNKNOWN) {
        SCLogDebug("expected command %" PRIu32 ", got %" PRIu32 ": ", FTP_COMMAND_UNKNOWN, ftp_state->command);
        result = 0;
        goto end;
    }

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    return result;
}

/** \test See how it deals with an incomplete request in multiple chunks. */
static int FTPParserTest07(void)
{
    int result = 1;
    Flow f;
    uint8_t ftpbuf1[] = "PO";
    uint32_t ftplen1 = sizeof(ftpbuf1) - 1; /* minus the \0 */
    uint8_t ftpbuf2[] = "RT\r\n";
    uint32_t ftplen2 = sizeof(ftpbuf2) - 1; /* minus the \0 */
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.alproto = ALPROTO_FTP;

    StreamTcpInitConfig(TRUE);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_FTP,
                                STREAM_TOSERVER | STREAM_START, ftpbuf1,
                                ftplen1);
    if (r != 0) {
        SCLogDebug("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        FLOWLOCK_UNLOCK(&f);
        goto end;
    }
    FLOWLOCK_UNLOCK(&f);

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_FTP,
                            STREAM_TOSERVER | STREAM_EOF, ftpbuf2, ftplen2);
    if (r != 0) {
        SCLogDebug("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        FLOWLOCK_UNLOCK(&f);
        goto end;
    }
    FLOWLOCK_UNLOCK(&f);

    FtpState *ftp_state = f.alstate;
    if (ftp_state == NULL) {
        SCLogDebug("no ftp state: ");
        result = 0;
        goto end;
    }

    if (ftp_state->command != FTP_COMMAND_PORT) {
        SCLogDebug("expected command %" PRIu32 ", got %" PRIu32 ": ",
                   FTP_COMMAND_PORT, ftp_state->command);
        result = 0;
        goto end;
    }

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    return result;
}

/** \test Test case where chunks are smaller than the delim length and the
  *       last chunk is supposed to match the delim. */
static int FTPParserTest10(void)
{
    int result = 1;
    Flow f;
    uint8_t ftpbuf1[] = "PORT 1,2,3,4,5,6\r\n";
    uint32_t ftplen1 = sizeof(ftpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();
    int r = 0;
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.alproto = ALPROTO_FTP;

    StreamTcpInitConfig(TRUE);

    uint32_t u;
    for (u = 0; u < ftplen1; u++) {
        uint8_t flags = 0;

        if (u == 0) flags = STREAM_TOSERVER|STREAM_START;
        else if (u == (ftplen1 - 1)) flags = STREAM_TOSERVER|STREAM_EOF;
        else flags = STREAM_TOSERVER;

        FLOWLOCK_WRLOCK(&f);
        r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_FTP, flags,
                                &ftpbuf1[u], 1);
        if (r != 0) {
            SCLogDebug("toserver chunk %" PRIu32 " returned %" PRId32 ", expected 0: ", u, r);
            result = 0;
            FLOWLOCK_UNLOCK(&f);
            goto end;
        }
        FLOWLOCK_UNLOCK(&f);
    }

    FtpState *ftp_state = f.alstate;
    if (ftp_state == NULL) {
        SCLogDebug("no ftp state: ");
        result = 0;
        goto end;
    }

    if (ftp_state->command != FTP_COMMAND_PORT) {
        SCLogDebug("expected command %" PRIu32 ", got %" PRIu32 ": ", FTP_COMMAND_PORT, ftp_state->command);
        result = 0;
        goto end;
    }

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    return result;
}
#endif /* UNITTESTS */

void FTPParserRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("FTPParserTest01", FTPParserTest01);
    UtRegisterTest("FTPParserTest03", FTPParserTest03);
    UtRegisterTest("FTPParserTest06", FTPParserTest06);
    UtRegisterTest("FTPParserTest07", FTPParserTest07);
    UtRegisterTest("FTPParserTest10", FTPParserTest10);
#endif /* UNITTESTS */
}

