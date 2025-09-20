<!doctype html>
<html lang="ro">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>Vestiar – Control uși</title>
<script src="https://unpkg.com/mqtt/dist/mqtt.min.js"></script>
<style>
  body {
    margin:0;
    font-family: Arial, sans-serif;
    background:#0b1220;
    color:#fff;
  }
  .wrap {
    max-width:800px;
    margin:0 auto;
    padding:10px;
  }
  .plan {
    position:relative;
    width:100%;
    background:#111;
    border:2px solid #333;
    border-radius:10px;
    overflow:hidden;
  }
  .plan img {
    width:100%;
    display:block;
  }
  .door {
    position:absolute;
    width:60px; height:30px;
    line-height:30px;
    text-align:center;
    font-size:12px;
    font-weight:bold;
    border-radius:4px;
    cursor:pointer;
    transition:0.3s;
  }
  .locked { background:#22c55e; }     /* verde */
  .unlocked { background:#ef4444; }   /* roșu */
  .nopower { background:#666; }       /* gri */
</style>
</head>
<body>
<div class="wrap">
  <h2>Control vestiar (6 uși)</h2>
  <div class="plan">
    <img src="vestiar.jpg" alt="Plan Vestiar" />

    <!-- Pozițiile ușilor -->
    <div id="door1" class="door locked" style="top:20%;left:10%">Ușa 1</div>
    <div id="door2" class="door locked" style="top:20%;left:40%">Ușa 2</div>
    <div id="door3" class="door locked" style="top:20%;left:70%">Ușa 3</div>
    <div id="door4" class="door locked" style="top:60%;left:10%">Ușa 4</div>
    <div id="door5" class="door locked" style="top:60%;left:40%">Ușa 5</div>
    <div id="door6" class="door locked" style="top:60%;left:70%">Ușa 6</div>
  </div>
</div>

<script>
(function(){
  const $ = id => document.getElementById(id);

  // Config MQTT
  const broker = "wss://broker.emqx.io:8084/mqtt";
  const options = { clientId:"web_"+Math.random().toString(16).substr(2,8) };
  const client = mqtt.connect(broker, options);

  const doors = {
    1: { el: $("door1"), topic: "locker/1/status", cmd:"locker/1/cmd" },
    2: { el: $("door2"), topic: "locker/2/status", cmd:"locker/2/cmd" },
    3: { el: $("door3"), topic: "locker/3/status", cmd:"locker/3/cmd" },
    4: { el: $("door4"), topic: "locker/4/status", cmd:"locker/4/cmd" },
    5: { el: $("door5"), topic: "locker/5/status", cmd:"locker/5/cmd" },
    6: { el: $("door6"), topic: "locker/6/status", cmd:"locker/6/cmd" },
  };

  client.on("connect", ()=>{
    console.log("MQTT conectat");
    Object.values(doors).forEach(d=> client.subscribe(d.topic));
  });

  client.on("message",(topic,msg)=>{
    const text = msg.toString();
    for (const id in doors){
      if (doors[id].topic === topic){
        const el = doors[id].el;
        el.classList.remove("locked","unlocked","nopower");
        if (text==="LOCKED") el.classList.add("locked");
        else if (text==="UNLOCKED") el.classList.add("unlocked");
        else el.classList.add("nopower");
        break;
      }
    }
  });

  // Click pe ușă => schimbă stare
  Object.values(doors).forEach(d=>{
    d.el.addEventListener("click", ()=>{
      let newCmd = d.el.classList.contains("locked") ? "UNLOCK" : "LOCK";
      client.publish(d.cmd, newCmd);
      console.log("Trimis", newCmd,"către", d.cmd);
    });
  });
})();
</script>
</body>
</html>
