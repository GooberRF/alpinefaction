# Alpine Faction Network Architecture

## Overview

Alpine Faction extends Red Faction's networking system with custom packets, enhanced security, and modern features while maintaining backward compatibility with the original protocol.

## Network Packet Flow

```mermaid
flowchart TB
    subgraph Client
        C1[Game Client]
        C2[Input Handler]
        C3[Packet Builder]
        C4[Network Layer]
    end
    
    subgraph Server
        S1[Network Handler]
        S2[Packet Validator]
        S3[Game State Manager]
        S4[Broadcast Manager]
    end
    
    subgraph "Packet Types"
        P1[Standard RF Packets]
        P2[Alpine Faction Packets]
        P3[PureFaction Packets]
    end
    
    C2 --> C3
    C3 --> C4
    C4 --> |Send| S1
    S1 --> S2
    S2 --> |Valid| S3
    S2 --> |Invalid| X[Drop Packet]
    S3 --> S4
    S4 --> |Broadcast| C4
    
    C3 --> P1
    C3 --> P2
    C3 --> P3
```

## Custom Packet System

```mermaid
sequenceDiagram
    participant Client
    participant Server
    participant Other Clients
    
    Note over Client,Server: Location Ping Example
    Client->>Server: af_ping_location_req (0x50)
    Server->>Server: Validate request
    Server->>Server: Check team membership
    Server->>Other Clients: af_ping_location (0x51)
    Other Clients->>Other Clients: Display ping on HUD
    
    Note over Client,Server: Damage Notification
    Client->>Server: Weapon fire
    Server->>Server: Calculate damage
    Server->>Client: af_damage_notify (0x52)
    Server->>Other Clients: af_damage_notify (0x52)
    Client->>Client: Play hit sound
    Client->>Client: Show damage numbers
```

## Packet Type Hierarchy

```mermaid
graph TD
    A[Red Faction Packets<br/>0x00-0x37] --> B[Standard Game Packets]
    A --> C[Entity Updates]
    A --> D[Player Actions]
    
    E[Alpine Faction Packets<br/>0x50+] --> F[af_ping_location_req 0x50]
    E --> G[af_ping_location 0x51]
    E --> H[af_damage_notify 0x52]
    E --> I[af_obj_update 0x53]
    
    J[PureFaction Packets] --> K[Anti-cheat Verification]
    J --> L[Player Statistics]
    J --> M[Server Authentication]
```

## Injection and Hook System

```mermaid
flowchart LR
    subgraph "Original Game Code"
        OG1[multi_io_process_packets]
        OG2[send_reliable_packet]
        OG3[obj_create_packet_update]
        OG4[game_info_packet]
    end
    
    subgraph "Alpine Faction Hooks"
        H1[Packet Processing Hook]
        H2[Send Packet Hook]
        H3[Object Update Hook]
        H4[Game Info Extension]
    end
    
    subgraph "Alpine Faction Code"
        AF1[process_custom_packet]
        AF2[enhanced_send_packet]
        AF3[obj_update_enhancement]
        AF4[append_af_signature]
    end
    
    OG1 --> H1 --> AF1
    OG2 --> H2 --> AF2
    OG3 --> H3 --> AF3
    OG4 --> H4 --> AF4
```

## System Integration Architecture

```mermaid
graph TB
    subgraph "Core Systems"
        NET[Network System]
        HUD[HUD System]
        SOUND[Sound System]
        INPUT[Input System]
    end
    
    subgraph "Alpine Features"
        PING[Location Pinging]
        DMG[Damage Indicators]
        STATS[Player Statistics]
        DLVL[Level Download]
    end
    
    subgraph "External Services"
        FF[FactionFiles.com]
        TRACKER[Server Tracker]
        CDN[Map CDN]
    end
    
    NET --> PING
    NET --> DMG
    NET --> STATS
    NET --> DLVL
    
    PING --> HUD
    DMG --> HUD
    DMG --> SOUND
    
    DLVL --> CDN
    STATS --> FF
    NET --> TRACKER
```

## Game Mode State Machine

```mermaid
stateDiagram-v2
    [*] --> Lobby: Server Start
    Lobby --> PreMatch: Match Start
    PreMatch --> InProgress: Countdown End
    
    state InProgress {
        [*] --> Playing
        Playing --> Respawning: Death
        Respawning --> Playing: Spawn
    }
    
    InProgress --> Overtime: Time Limit & Tied
    Overtime --> PostMatch: Winner Decided
    InProgress --> PostMatch: Game End
    PostMatch --> Lobby: Map Change
    
    note right of PreMatch: Ready System Active
    note right of Overtime: Sudden Death Rules
```

## PureFaction Integration

```mermaid
flowchart TD
    subgraph "PureFaction Anti-Cheat"
        PF1[Client Verification]
        PF2[Memory Scanning]
        PF3[Process Monitoring]
        PF4[Statistics Collection]
    end
    
    subgraph "Alpine Faction"
        AF1[Packet Handler]
        AF2[Player Manager]
        AF3[Server Config]
    end
    
    subgraph "Verification Flow"
        V1[Player Connects]
        V2[Send Challenge]
        V3[Verify Response]
        V4[Mark Status]
    end
    
    V1 --> PF1
    PF1 --> V2
    V2 --> V3
    V3 --> V4
    V4 --> AF2
    
    PF2 --> AF1
    PF3 --> AF1
    PF4 --> AF2
    AF3 --> PF1
```

## Server Configuration Flow

```mermaid
graph LR
    subgraph "Configuration Sources"
        CFG[dedicated_server.txt]
        CMD[Command Line Args]
        ADMIN[Admin Commands]
    end
    
    subgraph "Server Settings"
        ALPINE[Alpine-Only Mode]
        MATCH[Match Settings]
        SPAWN[Spawn Protection]
        VOTE[Vote Configuration]
        GAME[Game Modes]
    end
    
    subgraph "Runtime Behavior"
        ENFORCE[Client Requirements]
        PACKET[Packet Filtering]
        FEATURE[Feature Toggles]
    end
    
    CFG --> ALPINE
    CFG --> MATCH
    CFG --> SPAWN
    CFG --> VOTE
    CFG --> GAME
    
    CMD --> ALPINE
    ADMIN --> MATCH
    ADMIN --> VOTE
    
    ALPINE --> ENFORCE
    MATCH --> PACKET
    SPAWN --> FEATURE
    VOTE --> FEATURE
    GAME --> PACKET
```

## Damage Notification System

```mermaid
sequenceDiagram
    participant Attacker
    participant Server
    participant Victim
    participant Spectators
    
    Attacker->>Server: Fire weapon
    Server->>Server: Raycast/collision check
    Server->>Server: Calculate damage
    
    alt Hit registered
        Server->>Attacker: af_damage_notify<br/>(damage amount, death flag)
        Server->>Victim: Update health
        Server->>Spectators: af_damage_notify<br/>(for spectating)
        
        Attacker->>Attacker: Play hit sound
        Attacker->>Attacker: Show damage number
        
        alt Fatal damage
            Server->>All: Death notification
            Attacker->>Attacker: Play kill sound
        end
    end
```

## Level Download System

```mermaid
flowchart TD
    subgraph "Client"
        C1[Join Server]
        C2[Check Level]
        C3[Request Download]
        C4[Download Progress]
        C5[Load Level]
    end
    
    subgraph "Server"
        S1[Send Level Info]
        S2[Verify Request]
        S3[Send Download URL]
    end
    
    subgraph "CDN"
        CDN1[FactionFiles CDN]
        CDN2[Level Repository]
    end
    
    C1 --> S1
    S1 --> C2
    C2 --> |Missing| C3
    C2 --> |Have| C5
    C3 --> S2
    S2 --> S3
    S3 --> C4
    C4 --> CDN1
    CDN1 --> CDN2
    CDN2 --> C4
    C4 --> C5
```

## Enhanced Game Modes

```mermaid
graph TD
    subgraph "Standard Modes"
        DM[Deathmatch]
        TDM[Team Deathmatch]
        CTF[Capture the Flag]
    end
    
    subgraph "Alpine Faction Modes"
        GG[Gun Game]
        BAG[Bagman]
        MATCH[Match Mode]
    end
    
    subgraph "Mode Features"
        F1[Weapon Progression]
        F2[Bag Mechanics]
        F3[Ready System]
        F4[Overtime]
        F5[Team Balance]
    end
    
    GG --> F1
    BAG --> F2
    MATCH --> F3
    MATCH --> F4
    TDM --> F5
    CTF --> F5
```

## Packet Security Model

```mermaid
flowchart TB
    subgraph "Packet Reception"
        R1[Receive Packet]
        R2[Check Packet Type]
        R3[Validate Source]
        R4[Check Permissions]
    end
    
    subgraph "Validation"
        V1[Size Check]
        V2[Player ID Check]
        V3[Team Check]
        V4[State Check]
    end
    
    subgraph "Processing"
        P1[Process Packet]
        P2[Update Game State]
        P3[Broadcast Updates]
    end
    
    R1 --> R2
    R2 --> |Valid Type| R3
    R2 --> |Invalid| DROP[Drop Packet]
    R3 --> R4
    R4 --> V1
    V1 --> V2
    V2 --> V3
    V3 --> V4
    V4 --> P1
    P1 --> P2
    P2 --> P3
    
    V1 --> |Fail| DROP
    V2 --> |Fail| DROP
    V3 --> |Fail| DROP
    V4 --> |Fail| DROP
```

## Implementation Details

### Packet Structure

Alpine Faction packets follow this general structure:

```c++
struct AfPacketHeader {
    uint8_t type;      // Packet type (0x50+)
    uint8_t player_id; // Source player ID
    // Packet-specific data follows
};
```

### Hook Points

Key injection points in the original game:

1. **`multi_io_process_packets`** (0x0047918D) - Main packet processing
2. **`send_reliable_packet`** - Outbound packet interception
3. **`obj_create_packet_update`** - Enhanced object synchronization
4. **`game_info_packet`** - Server information extension

### Security Considerations

- All custom packets are validated for size and content
- Player IDs are verified against connection state
- Team-specific packets check team membership
- Server-only packets rejected from clients
- Buffer overflow protection on all packet reads

### Performance Optimizations

- Dynamic packet sizing based on player count
- Conditional updates only for changed state
- Efficient bit packing for boolean flags
- Batched updates where possible
- Client-side prediction for smooth gameplay