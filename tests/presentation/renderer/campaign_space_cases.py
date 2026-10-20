"""Shared stock campaign fixture for tactical-cycle and retained-save GPU proofs."""

import copy
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def fondor_campaign_arguments(game_root, directory):
    """Use installed types/maps and ordinary Move/Fight with a controlled three-faction campaign."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
    from tools.inventory.corpus import Corpus

    corpus = Corpus(game_root)
    campaign_path = "data/xml/campaigns_underworld_gc.xml"
    route_path = "data/xml/traderoutes.xml"
    campaigns = ET.fromstring(corpus.read_effective("foc", campaign_path).data)
    campaign = copy.deepcopy(next(row for row in campaigns
                                 if row.attrib.get("Name") == "Sandbox_Gateways_Rebel"))
    name = "EAWR_Fondor_Admission_Proof"
    campaign.set("Name", name)
    replace = {"Locations", "Trade_Routes", "Home_Location", "Starting_Active_Player",
               "Starting_Forces", "AI_Player_Control", "Special_Case_Production", "Campaign_Set"}
    for row in list(campaign):
        if row.tag in replace or row.tag.endswith("_Story_Name"):
            campaign.remove(row)
    def add(tag, value):
        ET.SubElement(campaign, tag).text = value
    add("Locations", "Bespin, Fondor, Byss")
    add("Trade_Routes", "EAWR_Fondor_Admission_Route")
    add("Starting_Active_Player", "Underworld")
    for faction, location in (("Underworld", "Bespin"), ("Rebel", "Fondor"), ("Empire", "Byss")):
        add("Home_Location", f"{faction}, {location}")
        add("AI_Player_Control", f"{faction}, None")
    for faction, location, unit in (("Underworld", "Bespin", "Underworld_Star_Base_1"),
                                    ("Underworld", "Bespin", "Tartan_Patrol_Cruiser"),
                                    ("Rebel", "Fondor", "Rebel_Star_Base_1"),
                                    ("Rebel", "Fondor", "Y-Wing_Squadron"),
                                    ("Empire", "Byss", "Empire_Star_Base_1")):
        add("Starting_Forces", f"{faction}, {location}, {unit}")
    campaigns.append(campaign)
    routes = ET.fromstring(corpus.read_effective("foc", route_path).data)
    route = copy.deepcopy(next(row for row in routes if row.attrib.get("Name") == "Bespin_Byss"))
    route.set("Name", "EAWR_Fondor_Admission_Route")
    route.find("Point_B").text = "Fondor"
    routes.append(route)
    mod = Path(directory) / "fondor-campaign"
    xml = mod / "Data" / "XML"
    xml.mkdir(parents=True)
    (xml.parent / "MegaFiles.xml").write_text(
        "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
    ET.ElementTree(campaigns).write(xml / "Campaigns_Underworld_GC.xml", encoding="utf-8")
    ET.ElementTree(routes).write(xml / "TradeRoutes.xml", encoding="utf-8")
    return ["--eawr-galactic", name, "--eawr-mod-root", str(mod),
            "--eawr-galactic-paused", "no", "--eawr-galactic-space-proof", "loss"]


def qualified_campaign_arguments(mode):
    """Select the authored Gateway route: Rebel Bespin fleet into Imperial Byss.

    Win/retreat submit ordinary merges of Bespin's two X-wing squadrons and
    Corellian corvette before moving. Loss/save keep the first single squadron.
    The Imperial Byss defender has a Tartan patrol cruiser and authored station.
    Tactical admission remains a production gate; this fixture never bypasses it.
    The native galactic AI (on by default since the 0.2.5 release default) is
    switched off: it launches every opposing fleet on the first ticks, leaving
    no authored defender in Byss orbit for the proof to attack.
    """
    if mode not in ("win", "retreat", "loss", "save", "prompt-save"):
        raise ValueError("unsupported campaign space proof mode: " + str(mode))
    return [
        "--eawr-galactic", "Sandbox_Gateways_Rebel",
        "--eawr-galactic-paused", "no",
        "--eawr-galactic-ai", "off",
        "--eawr-galactic-space-proof", mode,
    ]


def commander_campaign_arguments(game_root, directory):
    """Stock company creation, ordinary fleet merge/Move/Fight, two commanders."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
    from tools.inventory.corpus import Corpus

    corpus = Corpus(game_root)
    path = "data/xml/campaigns_underworld_gc.xml"
    campaigns = ET.fromstring(corpus.read_effective("foc", path).data)
    campaign = copy.deepcopy(next(row for row in campaigns
                                 if row.attrib.get("Name") == "Sandbox_Gateways_Rebel"))
    name = "EAWR_Commander_Admission_Proof"
    campaign.set("Name", name)
    for row in list(campaign):
        if row.tag in {"Starting_Forces", "AI_Player_Control", "Special_Case_Production", "Campaign_Set"} or row.tag.endswith("_Story_Name"):
            campaign.remove(row)
    for faction in ("Rebel", "Empire", "Underworld"):
        ET.SubElement(campaign, "AI_Player_Control").text = faction + ", None"
    for faction, planet, unit in (("Rebel", "Bespin", "Rebel_Star_Base_1"),
                                  ("Empire", "Byss", "Empire_Star_Base_1"),
                                  ("Rebel", "Bespin", "Corellian_Corvette"),
                                  ("Rebel", "Bespin", "Nebulon_B_Frigate"),
                                  ("Rebel", "Bespin", "Fleet_Com_Rebel_Team"),
                                  ("Rebel", "Bespin", "Fleet_Com_Rebel_Team"),
                                  ("Empire", "Byss", "Tartan_Patrol_Cruiser")):
        ET.SubElement(campaign, "Starting_Forces").text = f"{faction}, {planet}, {unit}"
    campaigns.append(campaign)
    mod = Path(directory) / "commander-campaign"
    xml = mod / "Data" / "XML"
    xml.mkdir(parents=True)
    (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
    ET.ElementTree(campaigns).write(xml / "Campaigns_Underworld_GC.xml", encoding="utf-8")
    return ["--eawr-galactic", name, "--eawr-mod-root", str(mod),
            "--eawr-galactic-paused", "no", "--eawr-galactic-space-proof", "retreat"]
