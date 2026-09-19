# UMQ Initialize

## Function Introduction
The Unified Message Queue (UMQ) initialization interface is responsible for loading the UMQ framework, configuring runtime parameters, and creating the initial data transmission environment. The UMQ environment must be correctly initialized before using any UMQ functionality.

## Overview
- Initialize each enabled UMQ framework; when built without `UMQ_STATIC_LIB`, load its corresponding shared object (`libumq_ub.so` for UB)
- Create and initialize devices based on the mode
- Register memory and initialize the small memory pool
- Obtain the transport DFX operations; start Perf recording separately with `umq_stats_perf_start()`

## Timing of Invocation
- UMQ must be initialized first when the application starts
- It only needs to be called once

## Configurable Parameter
- buf mode: Selects the mode for using UMQ Buffer, supports two types: 'UMQ_BUF_SPLIT' and 'UMQ_BUF_COMBINE'
- feature: Sets UMQ features
- | Feature Type                        | Description                                      |
  | ----------------------------------- | ------------------------------------------------ |
  | UMQ_FEATURE_API_BASE                | Use basic UMQ APIs                           |
  | UMQ_FEATURE_API_PRO                 | Use enhanced UMQ APIs                        |
  | UMQ_FEATURE_ENABLE_TOKEN_POLICY     | Enables the token policy                         |
  | UMQ_FEATURE_ENABLE_STATS            | Enables packet statistics                        |
  | UMQ_FEATURE_ENABLE_PERF             | Enables feature-gated latency probes; recording must be started separately |
  | UMQ_FEATURE_ENABLE_FLOW_CONTROL     | Enables flow control                             |
- headroom_size: Configures the size of the user data header, [0, 512] bytes
- io_lock_free: Can be set to true when I/O threads are safe to improve performance
- trans_info_num: The number of data plane transmission device information entries, supports up to 16
- flow_control: Configures flow control parameters
- buf_pool_cfg.small_block_size: Selects the small-block size using `BLOCK_SIZE_4K`, `BLOCK_SIZE_8K`, `BLOCK_SIZE_16K`, `BLOCK_SIZE_32K`, `BLOCK_SIZE_64K`, `BLOCK_SIZE_128K`, `BLOCK_SIZE_256K`, `BLOCK_SIZE_512K`, or `BLOCK_SIZE_1M`; the remaining buffer-pool configuration must also pass validation
- trans_info: Data plane transmission device information, used to initialize the corresponding device

# Description
1.When the UMQ_FEATURE_ENABLE_TOKEN_POLICY parameter is configured in UB mode, the UB token verification capability will be enabled. Consequently, umq_bind_info will carry UB token information. To ensure the secure transmission of this sensitive data, applications must exchange umq_bind_info over a secure channel, such as TLS.

## Device Initialization
Devices are initialized according to the transmission device information.

### UB Device Initialization
For UB devices, UMQ supports creating multiple devices using multiple EIDs simultaneously, with each device corresponding to one EID. UMQ allows users to initialize all devices at once via `umq_init` or to dynamically add devices after initialization. When initializing devices, users can specify either an `eid` or a `dev_name` + `eid_idx` combination. Set `dev_info.assign_mode` to match the selection data: use `UMQ_DEV_ASSIGN_MODE_EID` for `eid`, or `UMQ_DEV_ASSIGN_MODE_DEV` for `dev_name` + `eid_idx`. The `trans_info_num` determines the actual number of valid `trans_info` entries.

**One-time Initialization**

Execution Steps:

    1. Before initialization, prepare an array of `trans_info` for all EIDs that need to be initialized in `umq_init_cfg`, and correctly fill in `trans_info_num`
    2. Call the `umq_init` interface and pass in `umq_init_cfg`
    3. UMQ will create and initialize a corresponding UB device for each valid `trans_info`

One-time initialization UB device example:

```
    umq_init_cfg_t init_cfg = {
        .feature = UMQ_FEATURE_API_PRO,
        .trans_info_num = 2,
    };
    init_cfg.trans_info[0].trans_mode = UMQ_TRANS_MODE_UB;
    init_cfg.trans_info[0].dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    sprintf(init_cfg.trans_info[0].dev_info.dev.dev_name, "%s", "udma2");
    init_cfg.trans_info[0].dev_info.dev.eid_idx = 7;
    init_cfg.trans_info[1].trans_mode = UMQ_TRANS_MODE_UB;
    init_cfg.trans_info[1].dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    sprintf(init_cfg.trans_info[1].dev_info.dev.dev_name, "%s", "udma5");
    init_cfg.trans_info[1].dev_info.dev.eid_idx = 7;
    umq_init(&init_cfg);
```

**Dynamic Initialization**

Execution Steps:

    1. Prepare the `trans_info` for the new EID
    2. Call the `umq_dev_add` interface and pass in this `trans_info`
    3. UMQ will verify if this EID already exists, then create a new UB device for it, and register all previously registered memory pools onto this new device

Dynamic initialization UB device example:

```
    umq_trans_info_t trans_info = {0};
    trans_info.trans_mode = UMQ_TRANS_MODE_UB;
    trans_info.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    sprintf(trans_info.dev_info.dev.dev_name, "%s", "udma5");
    trans_info.dev_info.dev.eid_idx = 7;
    umq_dev_add(&trans_info);
```

## DFX Initialization
Currently, dfx supports the latency statistics (perf) module.

### Perf Initialization
Perf is a latency measurement module. Call `umq_stats_perf_start()` to initialize and start recording. On the first start, it allocates the recording context, initializes records and histograms for the default thread-ID range, and registers thread cleanup callbacks. Records for other threads are initialized on demand when a timestamp is requested. Probe points that check feature flags additionally require `UMQ_FEATURE_ENABLE_PERF`.
