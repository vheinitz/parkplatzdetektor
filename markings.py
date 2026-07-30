"""Markierungs-Detektion fuer die Drohnen-Draufsicht.

Kernidee: Fahrbahnmarkierungen sind *duenne* helle, entsaettigte Linien.
Autodaecher sind ebenfalls hell, aber quer dazu viel dicker. Ein Top-Hat mit
einem linienfoermigen Strukturelement quer zur gesuchten Richtung unterdrueckt
deshalb die Autos und laesst die Markierungen stehen.
"""
import cv2
import numpy as np

# Markierung ist ~4 px breit, ein Auto quer ~27 px -> SE dazwischen
SE_LEN = 15


def responses(img):
    """(resp_h, resp_v) -- Antwort auf horizontale bzw. vertikale Markierungen."""
    hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV)
    V = hsv[:, :, 2].astype(np.float32)
    S = hsv[:, :, 1].astype(np.float32)
    desat = np.clip(1.0 - S / 80.0, 0, 1)          # Markierungen sind grau/weiss

    se_v = cv2.getStructuringElement(cv2.MORPH_RECT, (1, SE_LEN))
    se_h = cv2.getStructuringElement(cv2.MORPH_RECT, (SE_LEN, 1))
    th_h = cv2.morphologyEx(V, cv2.MORPH_TOPHAT, se_v)   # horizontale Linien
    th_v = cv2.morphologyEx(V, cv2.MORPH_TOPHAT, se_h)   # vertikale Linien

    # entlang der Linie glaetten -> Luecken und Verdeckungen ueberbruecken
    resp_h = cv2.GaussianBlur(th_h * desat, (9, 1), 0)
    resp_v = cv2.GaussianBlur(th_v * desat, (1, 9), 0)
    return resp_h, resp_v
