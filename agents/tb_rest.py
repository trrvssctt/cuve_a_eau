"""
tb_rest.py — Acces REST a ThingsBoard.

Role : lire la derniere telemetrie du device ESP32 et lui envoyer des RPC.
(Equivalent du tb_rest.py du projet pompe-meteo-ml, adapte a CuveGuard.)
"""

import requests


class TbRest:
    """Client REST ThingsBoard avec re-login automatique sur JWT expire."""

    def __init__(self, host: str, username: str, password: str,
                 esp32_device_id: str):
        self.base = f"https://{host}"
        self.username = username
        self.password = password
        self.esp32_device_id = esp32_device_id
        self.jwt = None

    # ------------------------------------------------------------------
    def login(self) -> None:
        r = requests.post(
            f"{self.base}/api/auth/login",
            json={"username": self.username, "password": self.password},
            timeout=10,
        )
        r.raise_for_status()
        self.jwt = r.json()["token"]
        print("[TB-REST] Authentification OK")

    def _headers(self) -> dict:
        return {"X-Authorization": f"Bearer {self.jwt}"}

    def _request(self, method: str, url: str, **kwargs):
        r = requests.request(method, url, headers=self._headers(),
                             timeout=10, **kwargs)
        if r.status_code == 401:            # JWT expire -> re-login
            print("[TB-REST] JWT expire, re-authentification...")
            self.login()
            r = requests.request(method, url, headers=self._headers(),
                                 timeout=10, **kwargs)
        r.raise_for_status()
        return r

    # ------------------------------------------------------------------
    def lire_telemetrie(self, cles: list[str]) -> dict:
        """Retourne {cle: valeur} pour la derniere valeur de chaque cle."""
        url = (f"{self.base}/api/plugins/telemetry/DEVICE/"
               f"{self.esp32_device_id}/values/timeseries")
        r = self._request("GET", url, params={"keys": ",".join(cles)})
        data = r.json()
        return {c: data[c][0]["value"] for c in cles if data.get(c)}

    def envoyer_rpc(self, method: str, value) -> None:
        """RPC oneway vers l'ESP32 : {"method": ..., "params": {"value": ...}}"""
        url = f"{self.base}/api/plugins/rpc/oneway/{self.esp32_device_id}"
        self._request("POST", url, json={"method": method,
                                         "params": {"value": value}})
        print(f"[TB-REST] RPC {method}({value}) envoyee")
