"""
control_agent.py — Logique de decision de la pompe (seuils + hysteresis).

Role : a partir du niveau d'eau et de l'etat de la pompe, decider
START / STOP / HOLD. Aucune dependance reseau : pur calcul, testable seul.
(Equivalent du control_agent.py du projet pompe-meteo-ml.)
"""

from dataclasses import dataclass


@dataclass
class PumpDecision:
    decision: str            # "START" | "STOP" | "HOLD"
    commande: bool | None    # True/False si une RPC doit partir, None sinon


class ControlAgent:
    """
    Regles (sujet CuveGuard) :
      - niveau < seuil_start (30 %) et pompe arretee  -> START
      - niveau > seuil_stop  (90 %) et pompe en marche -> STOP
      - sinon -> HOLD

    La zone morte 30-90 % constitue l'hysteresis : une pompe demarree
    a 29 % ne s'arrete qu'a 90 %, donc aucun battement ON/OFF autour
    d'un seuil unique.
    """

    def __init__(self, seuil_start: float = 30.0, seuil_stop: float = 90.0):
        if seuil_start >= seuil_stop:
            raise ValueError("seuil_start doit etre < seuil_stop")
        self.seuil_start = seuil_start
        self.seuil_stop = seuil_stop

    def decider(self, level_pct: float, pump_on: bool) -> PumpDecision:
        if level_pct < self.seuil_start and not pump_on:
            return PumpDecision("START", True)
        if level_pct > self.seuil_stop and pump_on:
            return PumpDecision("STOP", False)
        return PumpDecision("HOLD", None)
