#!/bin/bash
set -e

MODEL_DIR=/workspace/models
SERVER_BIN=/workspace/FunASR/runtime/build/bin/funasr-wss-server

echo "[FunASR] Starting online ASR server (plain WebSocket, no SSL)..."
echo "[FunASR] Models directory: ${MODEL_DIR}"
echo "[FunASR] First run will auto-download models (~2GB), please wait..."

exec ${SERVER_BIN} \
    --download-model-dir ${MODEL_DIR} \
    --model-dir damo/speech_paraformer-large_asr_nat-zh-cn-16k-common-vocab8404-online \
    --vad-dir damo/speech_fsmn_vad_zh-cn_16k_common_pytorch \
    --punc-dir damo/punc_ct-transformer_zh-cn_common_vocab272727_pytorch \
    --decoder-thread-num 2 \
    --io-thread-num 2 \
    --port 10095
