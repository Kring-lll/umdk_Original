#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "urma_cmd.h"
#include "urma_cmd_tlv.h"
#include "udma_abi.h"   /* udma_create_ctx_resp（kernel_headers，内核安全） */

int main(void)
{
    /* OUT handle attr type（urma_cmd_tlv.h 各命令的 *_OUT_HANDLE 枚举）。
     * 值 = URMA_CMD_OUT_TYPE_INIT(0x80) + 偏移。打印确认。 */
    printf("URMA_CMD_OUT_TYPE_INIT = %d (0x%x)\n",
           URMA_CMD_OUT_TYPE_INIT, URMA_CMD_OUT_TYPE_INIT);
    printf("\n");
    printf("CREATE_JFC_OUT_HANDLE      = %d\n", CREATE_JFC_OUT_HANDLE);
    printf("CREATE_JFS_OUT_HANDLE      = %d\n", CREATE_JFS_OUT_HANDLE);
    printf("CREATE_JFR_OUT_HANDLE      = %d\n", CREATE_JFR_OUT_HANDLE);
    printf("CREATE_JETTY_OUT_HANDLE    = %d\n", CREATE_JETTY_OUT_HANDLE);
    printf("IMPORT_JETTY_OUT_HANDLE    = %d\n", IMPORT_JETTY_OUT_HANDLE);
    printf("IMPORT_JETTY_EX_OUT_HANDLE = %d\n", IMPORT_JETTY_EX_OUT_HANDLE);
    printf("BIND_JETTY_OUT_TPN         = %d\n", BIND_JETTY_OUT_TPN);
    printf("ALLOC_TOKEN_ID_OUT_TOKEN_ID= %d\n", ALLOC_TOKEN_ID_OUT_TOKEN_ID);
    printf("\n");
    /* CREATE_JFR 的 OUT_DEPTH / OUT_MAX_SGE（urma_sample 要） */
    printf("CREATE_JFR_OUT_DEPTH       = %d\n", CREATE_JFR_OUT_DEPTH);
    printf("CREATE_JFR_OUT_MAX_SGE    = %d\n", CREATE_JFR_OUT_MAX_SGE);
    /* CREATE_JETTY 的 IN_JFR_ID（共享 jfr_id，sim 建 jetty_id->jfr_id 映射要）+ SEND_JFC_ID */
    printf("CREATE_JETTY_IN_JFR_ID     = %d\n", CREATE_JETTY_IN_JFR_ID);
    printf("CREATE_JETTY_IN_SEND_JFC_ID= %d\n", CREATE_JETTY_IN_SEND_JFC_ID);
    /* REGISTER_SEG OUT_TOKEN_ID（sim 回填=IN_TOKEN_ID） */
    printf("REGISTER_SEG_OUT_TOKEN_ID = %d\n", REGISTER_SEG_OUT_TOKEN_ID);
    printf("REGISTER_SEG_IN_TOKEN_ID  = %d\n", REGISTER_SEG_IN_TOKEN_ID);
    /* CREATE_CTX 的 OUT_UDATA/OUT_ASYNC_FD */
    printf("CREATE_CTX_OUT_UDATA       = %d\n", CREATE_CTX_OUT_UDATA);
    printf("CREATE_CTX_OUT_ASYNC_FD    = %d\n", CREATE_CTX_OUT_ASYNC_FD);
    /* CREATE_JFCE 回填（eventfd） */
    printf("CREATE_JFCE_OUT_FD        = %d\n", CREATE_JFCE_OUT_FD);
    /* QUERY_DEV_ATTR 关键 OUT（前几个） */
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC  = %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS  = %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR  = %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR);
    printf("QUERY_DEVICE_OUT_GUID     = %d\n", QUERY_DEVICE_OUT_GUID);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC_DEPTH= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC_DEPTH);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_DEPTH= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_DEPTH);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_DEPTH= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_DEPTH);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_SGE= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_SGE);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_SGE= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_SGE);
    printf("QUERY_DEVICE_OUT_DEV_CAP_TRANS_MODE= %d\n", QUERY_DEVICE_OUT_DEV_CAP_TRANS_MODE);
    printf("QUERY_DEVICE_OUT_DEV_CAP_ATOMIC_FEAT= %d\n", QUERY_DEVICE_OUT_DEV_CAP_ATOMIC_FEAT);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_MSG_SIZE= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_MSG_SIZE);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_READ_SIZE= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_READ_SIZE);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_WRITE_SIZE= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_WRITE_SIZE);
    printf("QUERY_DEVICE_OUT_PORT_CNT= %d\n", QUERY_DEVICE_OUT_PORT_CNT);
    printf("QUERY_DEVICE_OUT_DEV_CAP_PAGE_SIZE_CAP= %d\n", QUERY_DEVICE_OUT_DEV_CAP_PAGE_SIZE_CAP);
    printf("QUERY_DEVICE_OUT_DEV_CAP_MAX_EID_CNT= %d\n", QUERY_DEVICE_OUT_DEV_CAP_MAX_EID_CNT);
    printf("sizeof(urma_cmd_udrv_priv_t) = %zu\n", sizeof(urma_cmd_udrv_priv_t));
    printf("CREATE_JFR_IN_DEPTH   = %d\n", CREATE_JFR_IN_DEPTH);
    printf("CREATE_JFR_IN_MAX_SGE= %d\n", CREATE_JFR_IN_MAX_SGE);
    printf("CREATE_JFR_IN_JFC_ID = %d\n", CREATE_JFR_IN_JFC_ID);
    printf("REGISTER_SEG_IN_TOKEN_ID = %d\n", REGISTER_SEG_IN_TOKEN_ID);
    printf("REGISTER_SEG_OUT_TOKEN_ID= %d\n", REGISTER_SEG_OUT_TOKEN_ID);
    return 0;
}
