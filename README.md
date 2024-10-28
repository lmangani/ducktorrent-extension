# Ducktorrent

> Nothing to see here, move on

Basic node discovery experiment. Do not use. 

### NODE A
```sql
D SELECT announce_presence('test12345');
┌────────────────────────────────┐
│ announce_presence('test12345') │
│            varchar             │
├────────────────────────────────┤
│ Announced                      │
└────────────────────────────────┘

D SELECT find_peers('test12345');
┌─────────────────────────────────────────────────────┐
│               find_peers('test12345')               │
│                       varchar                       │
├─────────────────────────────────────────────────────┤
│ [{"ip":"\19","port":50327,"user_data":"test12345"}] │
└─────────────────────────────────────────────────────┘
```

### Node B
```sql
D SELECT announce_presence('test12345');
┌────────────────────────────────┐
│ announce_presence('test12345') │
│            varchar             │
├────────────────────────────────┤
│ Announced                      │
└────────────────────────────────┘

D SELECT find_peers('test12345');
┌─────────────────────────────────────────────────────┐
│               find_peers('test12345')               │
│                       varchar                       │
├─────────────────────────────────────────────────────┤
│ [{"ip":"\19","port":52676,"user_data":"test12345"}] │
└─────────────────────────────────────────────────────┘
```
