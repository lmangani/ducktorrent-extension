# Ducktorrent

> Nothing to see here, move on

Experimental extension for DHT node discovery. Do not use. 

### Example
Start the local DHT node
```sql
D SELECT dht_start(8991);
```

Announce presence using a hash `token`
```sql
D SELECT announce_presence('fa8f6d21eeb3948b8497439b4d540294c42653d1');
```

Find peers from any node using the same hash `token`

```sql
D SELECT find_peers('fa8f6d21eeb3948b8497439b4d540294c42653d1');
```

