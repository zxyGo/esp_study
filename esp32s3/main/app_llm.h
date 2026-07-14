#pragma once

/* 启动后台 HTTP 客户端任务。重复调用是安全的。 */
void app_llm_start(void);

/* 提交一句最终识别文本；队列中已有未处理问题时用新问题替换。 */
void app_llm_ask(const char *text);
