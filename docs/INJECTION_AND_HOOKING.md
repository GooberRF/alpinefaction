# Alpine Faction Injection and Hooking System

## Overview

Alpine Faction uses sophisticated runtime code modification techniques to enhance Red Faction without access to its source code. This document details the injection framework, hooking mechanisms, and how they integrate to create new functionality.

## System Architecture

```mermaid
flowchart TB
    subgraph "Launcher Process"
        L1[AlpineFactionLauncher.exe]
        L2[DLL Injector]
        L3[Process Creator]
    end
    
    subgraph "Game Process"
        G1[RF.exe/RED.exe]
        G2[Original Code]
        G3[Injected DLLs]
        G4[Hook Framework]
        G5[Modified Behavior]
    end
    
    subgraph "Injected Components"
        D1[DashFactionGame.dll]
        D2[DashFactionEditor.dll]
        D3[CrashHandlerStub.dll]
    end
    
    L1 --> L3
    L3 --> G1
    L2 --> D1
    L2 --> D2
    L2 --> D3
    
    D1 --> G3
    D2 --> G3
    D3 --> G3
    
    G3 --> G4
    G4 --> G2
    G2 --> G5
```

## Injection Process

```mermaid
sequenceDiagram
    participant Launcher
    participant Windows
    participant GameProcess
    participant InjectedDLL
    
    Launcher->>Windows: CreateProcess(SUSPENDED)
    Windows->>GameProcess: Process created (suspended)
    Launcher->>GameProcess: Allocate memory
    Launcher->>GameProcess: Write DLL path
    Launcher->>Windows: CreateRemoteThread(LoadLibrary)
    Windows->>GameProcess: Execute LoadLibrary
    GameProcess->>InjectedDLL: DLL loaded
    InjectedDLL->>InjectedDLL: DllMain executes
    InjectedDLL->>GameProcess: Install hooks
    Launcher->>GameProcess: Resume main thread
    GameProcess->>GameProcess: Game starts with hooks
```

## Hook Types and Implementation

### 1. Function Hooks (FunHook)

```mermaid
flowchart LR
    subgraph "Original Function"
        OF[Original Code]
        OR[Original Return]
    end
    
    subgraph "Hook Implementation"
        HF[Hook Function]
        HC[Custom Code]
        HT[Trampoline]
    end
    
    subgraph "Execution Flow"
        CALL[Function Call] --> HF
        HF --> HC
        HC --> |Optional| HT
        HT --> OF
        OF --> OR
        HC --> |Override| OR
    end
```

**Example Implementation:**
```cpp
FunHook<void(int, const char*)> log_hook{
    0x00505F70,  // Original function address
    [](int level, const char* msg) {
        // Custom preprocessing
        process_log_message(msg);
        
        // Call original function
        log_hook.call_target(level, msg);
        
        // Custom postprocessing
        send_to_external_logger(msg);
    }
};
```

### 2. Call Hooks (CallHook)

```mermaid
flowchart TD
    subgraph "Original Code"
        O1[Instruction 1]
        O2[CALL original_func]
        O3[Instruction 3]
    end
    
    subgraph "After Hook"
        M1[Instruction 1]
        M2[CALL hook_func]
        M3[Instruction 3]
    end
    
    O1 --> O2
    O2 --> O3
    M1 --> M2
    M2 --> M3
    
    O2 -.->|Replaced| M2
```

**Example Implementation:**
```cpp
CallHook player_spawn_hook{
    0x0045B7A2,  // Address of CALL instruction
    [](rf::Player* player) {
        original_spawn(player);
        
        // Alpine Faction additions
        apply_spawn_protection(player);
        trigger_spawn_effects(player);
        update_scoreboard(player);
    }
};
```

### 3. Code Injection

```mermaid
flowchart TB
    subgraph "Original Function"
        START[Function Start]
        CODE1[Original Code Block 1]
        CODE2[Original Code Block 2]
        END[Function End]
    end
    
    subgraph "Injected Code"
        INJ1[Save Registers]
        INJ2[Custom Logic]
        INJ3[Restore Registers]
        JMP[Jump Back]
    end
    
    START --> CODE1
    CODE1 --> |Detour| INJ1
    INJ1 --> INJ2
    INJ2 --> INJ3
    INJ3 --> JMP
    JMP --> CODE2
    CODE2 --> END
```

**Example Implementation:**
```cpp
CodeInjection damage_calc_injection{
    0x00418B45,  // Injection point
    [](auto& regs) {
        float damage = regs.eax;
        
        // Apply Alpine Faction damage modifiers
        damage *= get_damage_multiplier();
        
        if (is_critical_hit()) {
            damage *= 2.0f;
            play_critical_sound();
        }
        
        regs.eax = damage;
    }
};
```

## Memory Management

```mermaid
graph TD
    subgraph "Alpine Memory Regions"
        A1[Code Caves]
        A2[Trampolines]
        A3[Hook Tables]
        A4[Data Storage]
    end
    
    subgraph "Protection Management"
        P1[VirtualProtect]
        P2[PAGE_EXECUTE_READWRITE]
        P3[Restore Protection]
    end
    
    subgraph "Allocation Strategy"
        S1[Find Free Space]
        S2[Allocate Near Target]
        S3[5-byte Jump Range]
    end
    
    A1 --> P1
    A2 --> P2
    P2 --> P3
    
    S1 --> A1
    S2 --> A2
    S3 --> A2
```

## Virtual Table Patching

```mermaid
flowchart LR
    subgraph "Original VTable"
        V1[Method1 Ptr]
        V2[Method2 Ptr]
        V3[Method3 Ptr]
    end
    
    subgraph "Patched VTable"
        P1[Method1 Ptr]
        P2[Hook2 Ptr]
        P3[Method3 Ptr]
    end
    
    subgraph "Hook Implementation"
        H1[Hook Function]
        H2[Custom Logic]
        H3[Call Original]
    end
    
    V2 -.->|Replace| P2
    P2 --> H1
    H1 --> H2
    H2 --> H3
    H3 --> V2
```

## Hook Installation Process

```mermaid
stateDiagram-v2
    [*] --> Identify: Find target addresses
    Identify --> Analyze: Disassemble code
    Analyze --> Prepare: Create hook function
    Prepare --> Protect: Change memory protection
    Protect --> Write: Write jump/call
    Write --> Restore: Restore protection
    Restore --> Verify: Test hook
    Verify --> [*]: Hook active
    
    Analyze --> Abort: Invalid target
    Write --> Rollback: Write failed
    Rollback --> Abort
    Abort --> [*]
```

## Common Hook Patterns

### 1. Render Pipeline Hooks

```mermaid
flowchart TD
    subgraph "Original Render"
        R1[Begin Frame]
        R2[Render Objects]
        R3[End Frame]
        R4[Present]
    end
    
    subgraph "Alpine Hooks"
        H1[Pre-render Hook]
        H2[Object Filter]
        H3[Post-process]
        H4[HUD Overlay]
    end
    
    R1 --> H1
    H1 --> R2
    R2 --> H2
    H2 --> R3
    R3 --> H3
    H3 --> H4
    H4 --> R4
```

### 2. Input Processing Hooks

```mermaid
flowchart LR
    subgraph "Input Pipeline"
        I1[Raw Input]
        I2[Process Input]
        I3[Game Action]
    end
    
    subgraph "Alpine Additions"
        A1[Input Filter]
        A2[New Bindings]
        A3[Action Mapper]
    end
    
    I1 --> A1
    A1 --> I2
    I2 --> A2
    A2 --> A3
    A3 --> I3
```

### 3. Network Packet Hooks

```mermaid
flowchart TB
    subgraph "Packet Flow"
        P1[Receive Packet]
        P2[Parse Header]
        P3[Process Packet]
    end
    
    subgraph "Hook Points"
        H1[Pre-receive Hook]
        H2[Header Hook]
        H3[Custom Handler]
    end
    
    P1 --> H1
    H1 --> P2
    P2 --> H2
    H2 --> |Custom Type| H3
    H2 --> |Standard| P3
```

## Advanced Techniques

### 1. Hot Patching

```mermaid
flowchart LR
    subgraph "Runtime Modification"
        RT1[Detect State]
        RT2[Suspend Threads]
        RT3[Modify Code]
        RT4[Resume Threads]
    end
    
    RT1 --> RT2
    RT2 --> RT3
    RT3 --> RT4
    RT4 --> RT1
```

### 2. Detour Chains

```mermaid
graph TD
    ORIG[Original Function]
    H1[Hook 1]
    H2[Hook 2]
    H3[Hook 3]
    
    CALL[Function Call] --> H3
    H3 --> |Chain| H2
    H2 --> |Chain| H1
    H1 --> |Chain| ORIG
    
    H3 --> |Override| RETURN[Return]
```

### 3. Safe Hooking

```mermaid
flowchart TB
    subgraph "Safety Checks"
        S1[Verify Signature]
        S2[Check Version]
        S3[Validate Address]
        S4[Test Hook]
    end
    
    subgraph "Fallback"
        F1[Log Error]
        F2[Disable Feature]
        F3[Use Alternative]
    end
    
    S1 --> |Pass| S2
    S2 --> |Pass| S3
    S3 --> |Pass| S4
    S4 --> |Pass| SUCCESS[Install Hook]
    
    S1 --> |Fail| F1
    S2 --> |Fail| F1
    S3 --> |Fail| F1
    S4 --> |Fail| F1
    
    F1 --> F2
    F2 --> F3
```

## Performance Considerations

### Hook Overhead

```mermaid
graph LR
    subgraph "Performance Impact"
        O1[Original: 5 cycles]
        O2[Simple Hook: +10 cycles]
        O3[Complex Hook: +50 cycles]
        O4[Chain Hook: +20n cycles]
    end
    
    O1 --> O2
    O2 --> O3
    O3 --> O4
```

### Optimization Strategies

1. **Inline Assembly** - Minimize register saves
2. **Conditional Hooks** - Only execute when needed
3. **Batch Operations** - Group related modifications
4. **Cache Friendly** - Keep hook code in same page

## Debugging Hooks

```mermaid
flowchart TD
    subgraph "Debug Infrastructure"
        D1[Hook Logger]
        D2[Call Counter]
        D3[Performance Timer]
        D4[Stack Tracer]
    end
    
    subgraph "Debug Output"
        O1[Console Log]
        O2[File Log]
        O3[Debug View]
    end
    
    D1 --> O1
    D1 --> O2
    D2 --> O3
    D3 --> O3
    D4 --> O2
```

## Security Implications

### Anti-Tamper Detection

```mermaid
flowchart LR
    subgraph "Detection Methods"
        DM1[Checksum Validation]
        DM2[Hook Detection]
        DM3[Module Scanning]
    end
    
    subgraph "Alpine Response"
        AR1[Signature Spoofing]
        AR2[Stealth Hooks]
        AR3[Whitelisting]
    end
    
    DM1 --> AR1
    DM2 --> AR2
    DM3 --> AR3
```

### Protection Mechanisms

1. **Code Integrity** - Verify hook targets before installation
2. **Access Control** - Limit hook installation to initialization
3. **Validation** - Check hook results for sanity
4. **Isolation** - Separate hook code from game memory

## Best Practices

1. **Always Save Registers** - Preserve CPU state
2. **Check Boundaries** - Validate memory access
3. **Handle Failures** - Graceful degradation
4. **Document Hooks** - Clear purpose and dependencies
5. **Version Specific** - Account for game updates
6. **Thread Safety** - Consider concurrent access
7. **Performance Test** - Measure impact
8. **Reversibility** - Support unhooking