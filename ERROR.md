# PotatoStream

<img width="512" height="256" alt="banner" src="https://github.com/user-attachments/assets/638755f4-deb1-4535-98da-36d30f4848d9" />
<br><br>

**PotatoStream** is a game streaming client for **all Nintendo 3DS and 2DS models**, forked from [moonlight-N3DS](https://github.com/zoeyjodon/moonlight-N3DS) by zoeyjodon. Built with a focus on making streaming actually usable on **Old 3DS, Old 3DS XL and 2DS**, but works on New 3DS and New 2DS XL too.

Compatible with [Sunshine](https://github.com/LizardByte/Sunshine) (open-source, recommended) and NVIDIA GameStream.

> The original project targets the *New* 3DS and its hardware MVD decoder. PotatoStream keeps full New 3DS support while adding a dedicated Potato mode for older hardware: ARM11 compiler optimizations, smart frame skipping, auto-configured stream profile, and native Y2RU video pipeline.

---

## Support

For any problem, question or bug report, please go through the Discord - you will get a much faster answer there than through GitHub issues:

**[discord.gg/bgHwErJUtp](https://discord.gg/bgHwErJUtp)**

---

## Error Codes

| Code | Name | Cause | Fix |
|------|------|-------|-----|
| 13 | EACCES | RTP port unavailable, often a previous stream that wasn't properly released | Quit and relaunch the app |
| 104 | ECONNRESET | Host disconnected | Check that Sunshine is still running |
| 105 | ENOBUFS | SOCU pool exhausted | Relaunch the app |
| 111 | ECONNREFUSED | Nothing is listening on the port | Sunshine stopped or firewall blocking it |
| 114 | ENETUNREACH | No route to host | 3DS Wi-Fi is outside the host's network |
| 116 | ETIMEDOUT | No response | Host frozen, or connection dropped server-side |
| 118 | EHOSTUNREACH | Host unreachable | Wrong IP address |