# Ducktorrent

> Nothing to see here, move on

Experimental extension for Local network UDP discovery. Do not use. 

### Example
Announce presence from multiple instances using the same `token`
```sql
D SELECT announce_presence('test12345');
┌────────────────────────────────┐
│ announce_presence('test12345') │
│            varchar             │
├────────────────────────────────┤
│ Announced                      │
└────────────────────────────────┘
```

Find peers from any node using the same `token` to receive peers + metadata

```sql
D SELECT find_peers('test12345');
┌───────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                         find_peers('test12345')                                   │
│                                               varchar                                             │
├───────────────────────────────────────────────────────────────────────────────────────────────────┤
│ [{"ip":"xxx.xxx.xxx.xx","port":44802,"user_data": {"custom":"anything","user_data":"test12345"} } │
└───────────────────────────────────────────────────────────────────────────────────────────────────┘
```

