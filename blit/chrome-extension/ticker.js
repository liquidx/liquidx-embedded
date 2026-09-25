// A 1 s heartbeat for the session page. Timers in worker threads aren't
// subject to the throttling Chrome applies to hidden pages, so updates keep
// their pace when the session window is behind others or minimised.
setInterval(() => postMessage(Date.now()), 1000);
