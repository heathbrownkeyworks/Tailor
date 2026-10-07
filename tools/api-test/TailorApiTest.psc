;/* TailorApiTest
* * a quest script that calls every function of Tailor's mod API (Tailor.psc) on the player and on one NPC,
* * writes each call and its result to the Papyrus log, and shows a summary on screen
* * it is a tester's tool, not part of Tailor: nothing here ships with the mod
* * it comes with TailorApiTestPlayerAlias.psc, which listens for Tailor's events again after each load
* *
* * HOW TO SET IT UP
* * 1. compile both scripts with the Creation Kit's Papyrus compiler, with papyrus\Source\Scripts (where
* *    Tailor.psc is) and the SKSE script sources on the import path, and copy both .pex files to
* *    Data\Scripts
* * 2. in the Creation Kit, make a new quest (any editor ID, for example TailorApiTestQuest): on the Quest Data
* *    tab check "Start Game Enabled", on the Scripts tab add TailorApiTest, select it and click Properties,
* *    and set TestNpc to an NPC reference placed in the world (for example Lydia's), whom you can find in game
* * 3. on the quest's Quest Aliases tab, add a Reference Alias (any name, for example Player), set its Fill
* *    Type to Specific Reference and pick PlayerRef, and add TailorApiTestPlayerAlias to the alias's script
* *    list; without it the test stops listening for the events after a load
* * 4. save the plugin as an ESP and enable it in a load order with Tailor 3.0 or newer
* * 5. turn on Papyrus logging in Skyrim.ini ([Papyrus] bEnableLogging=1, bEnableTrace=1) and read
* *    Documents\My Games\Skyrim Special Edition\Logs\Script\Papyrus.0.log, each line starts with
* *    [TailorApiTest]
* *
* * WHAT IT DOES
* * - five seconds after the quest first starts (a new game, or the first time a save loads with the plugin;
* *   not at every load) it runs every group of calls once, which takes about 5 seconds, or 10 with TestNpc,
* *   and shows how many checks came out as expected
* * - F10 runs everything again; press it after loading a save to run it again there
* * - F11 keeps the test outfit as an override on the player and TestNpc until you press F11 again, so you can
* *   walk into a bed, water, cold weather or a fight and check that it holds (docs\testing\MOD_API.md)
* * - it listens for both Tailor events and writes each one to the log; the ones about the player and TestNpc
* *   also show on screen. The events and the two keys are registered when the quest starts, at each run, at
* *   F11, and after each load by the alias script, since SKSE forgets mod event registrations at a load (key
* *   registrations last through a load, and registering them again does no harm)
* *
* * WHAT IT LEAVES BEHIND, AND WHAT IT CHANGES
* * - an outfit "TailorApiTest Outfit" (iron armor and iron boots) in a category "TailorApiTest Category", in the
* *   player's Tailor library, which every save and character share and Tailor's API cannot delete; delete them
* *   in Tailor's screens when you're done (the second run onward expects the first to have made them)
* * - running everything first clears any override on the player and on TestNpc, whichever mod set it, and
* *   leaves none behind; while it runs, the player and TestNpc are briefly dressed in the test outfit
*/;
Scriptname TailorApiTest extends Quest

Actor Property TestNpc Auto
String Property OutfitName = "TailorApiTest Outfit" AutoReadOnly
String Property CategoryName = "TailorApiTest Category" AutoReadOnly
Int Property RunKey = 68 AutoReadOnly   ; F10
Int Property HoldKey = 87 AutoReadOnly   ; F11

Int _calls
Int _expected
Int _met
Bool _running


Event OnInit()
	RegisterEvents()
	RegisterForSingleUpdate(5.0)
EndEvent

Event OnUpdate()
	RunAll()
EndEvent

Event OnKeyDown(Int aiKeyCode)
	If aiKeyCode == RunKey
		RunAll()
	ElseIf aiKeyCode == HoldKey
		ToggleHold()
	EndIf
EndEvent


;  EVENTS

; Tailor's two events and the two keys. SKSE forgets mod event registrations at a load, as Tailor.psc says (key
; registrations last, and registering them again does no harm), so this runs when the quest starts, at each run and
; at F11, and TailorApiTestPlayerAlias calls it after each load
Function RegisterEvents()
	RegisterForKey(RunKey)
	RegisterForKey(HoldKey)
	RegisterForModEvent("Tailor_OnOutfitChanged", "OnTailorOutfitChanged")
	RegisterForModEvent("Tailor_OnSituationChanged", "OnTailorSituationChanged")
EndFunction

Event OnTailorOutfitChanged(String asEventName, String asOutfit, Float afNumArg, Form akSender)
	Actor akWearer = akSender as Actor
	Debug.Trace("[TailorApiTest] " + asEventName + ": " + NameOf(akWearer) + " wears '" + asOutfit + "'")
	If akWearer && (akWearer == Game.GetPlayer() || akWearer == TestNpc)
		If asOutfit == ""
			Debug.Notification(NameOf(akWearer) + ": own gear")
		Else
			Debug.Notification(NameOf(akWearer) + ": outfit " + asOutfit)
		EndIf
	EndIf
EndEvent

Event OnTailorSituationChanged(String asEventName, String asSituation, Float afNumArg, Form akSender)
	Actor akPerson = akSender as Actor
	Debug.Trace("[TailorApiTest] " + asEventName + ": " + NameOf(akPerson) + " is in '" + asSituation + "'")
	If akPerson && (akPerson == Game.GetPlayer() || akPerson == TestNpc)
		Debug.Notification(NameOf(akPerson) + ": situation " + asSituation)
	EndIf
EndEvent


;  THE RUN

Function RunAll()
	If _running
		Return
	EndIf
	If Tailor.GetApiVersion() < 1.0
		Debug.Notification("Tailor API test: Tailor 3.0 or newer is not installed")
		Debug.Trace("[TailorApiTest] Tailor.GetApiVersion() is below 1.0: Tailor 3.0 or newer is not installed")
		Return
	EndIf
	_running = True
	_calls = 0
	_expected = 0
	_met = 0
	RegisterEvents()
	If TestNpc
		Debug.Notification("Tailor API test: running, about 10 seconds")
	Else
		Debug.Notification("Tailor API test: running, about 5 seconds")
	EndIf
	ExpectBool("GetApiVersion() >= 1.0", Tailor.GetApiVersion() >= 1.0, True)
	If !TestNpc
		Debug.Trace("[TailorApiTest] TestNpc is not set: the checks on an NPC are skipped")
	EndIf
	; building first: the other groups use the outfit and the category it makes
	TestBuilding()
	TestOutfits()
	TestLists()
	TestSituations()
	TestOverrides()
	TestUtility()
	Debug.Notification("Tailor API test: " + _met + " of " + _expected + " checks as expected, " + _calls + " calls (see the Papyrus log)")
	Debug.Trace("[TailorApiTest] done: " + _met + " of " + _expected + " checks as expected, " + _calls + " calls")
	_running = False
EndFunction

; F11: with an override on the player or TestNpc, clears both; otherwise sets the test outfit on both
Function ToggleHold()
	If Tailor.GetApiVersion() < 1.0
		Return
	EndIf
	RegisterEvents()
	Actor akPlayer = Game.GetPlayer()
	If Tailor.HasOutfitOverride(akPlayer) || (TestNpc && Tailor.HasOutfitOverride(TestNpc))
		Note("ClearOutfitOverride(player)", BoolText(Tailor.ClearOutfitOverride(akPlayer)))
		If TestNpc
			Note("ClearOutfitOverride(TestNpc)", BoolText(Tailor.ClearOutfitOverride(TestNpc)))
		EndIf
		Debug.Notification("Tailor API test: overrides cleared")
	Else
		Note("OverrideWithOutfit(player, " + OutfitName + ")", BoolText(Tailor.OverrideWithOutfit(akPlayer, OutfitName)))
		If TestNpc
			Note("OverrideWithOutfit(TestNpc, " + OutfitName + ")", BoolText(Tailor.OverrideWithOutfit(TestNpc, OutfitName)))
		EndIf
		Debug.Notification("Tailor API test: overrides set, F11 clears them")
	EndIf
EndFunction


;  BUILDING: CreateOutfit, AddArmorToOutfit, RemoveArmorFromOutfit, CreateCustomCategory, AddCategory

Function TestBuilding()
	Armor akCuirass = Game.GetForm(0x00012E49) as Armor   ; Skyrim.esm: iron armor
	Armor akBoots = Game.GetForm(0x00012E4B) as Armor   ; Skyrim.esm: iron boots
	; the library keeps what a run built, so only the first run creates the outfit and the category
	Bool bOutfitExisted = Tailor.DoesOutfitExist(OutfitName)
	Bool bCategoryExisted = Tailor.DoesCategoryExist(CategoryName)

	ExpectBool("CreateCustomCategory(" + CategoryName + ")", Tailor.CreateCustomCategory(CategoryName), !bCategoryExisted)
	ExpectBool("CreateCustomCategory(" + CategoryName + ") again", Tailor.CreateCustomCategory(CategoryName), False)
	ExpectBool("CreateCustomCategory(Warm), a built-in name", Tailor.CreateCustomCategory("Warm"), False)

	ExpectBool("CreateOutfit(" + OutfitName + ")", Tailor.CreateOutfit(OutfitName, "", -1), !bOutfitExisted)
	ExpectBool("CreateOutfit(" + OutfitName + ") again", Tailor.CreateOutfit(OutfitName, "", -1), False)
	ExpectBool("CreateOutfit(TAILORAPITEST OUTFIT), capitals ignored", Tailor.CreateOutfit("TAILORAPITEST OUTFIT", "", -1), False)
	ExpectBool("CreateOutfit(a new name, no such category)", Tailor.CreateOutfit("TailorApiTest Other", "No Such Category", -1), False)
	ExpectBool("CreateOutfit(a new name, gender 2)", Tailor.CreateOutfit("TailorApiTest Other", "", 2), False)

	ExpectBool("AddArmorToOutfit(iron armor)", Tailor.AddArmorToOutfit(OutfitName, akCuirass), True)
	ExpectBool("AddArmorToOutfit(iron boots)", Tailor.AddArmorToOutfit(OutfitName, akBoots), True)
	ExpectBool("AddArmorToOutfit(iron armor) again, the outfit has it", Tailor.AddArmorToOutfit(OutfitName, akCuirass), True)
	ExpectBool("AddArmorToOutfit(None)", Tailor.AddArmorToOutfit(OutfitName, None), False)
	ExpectBool("AddArmorToOutfit(no such outfit)", Tailor.AddArmorToOutfit("No Such Outfit", akCuirass), False)
	ExpectBool("RemoveArmorFromOutfit(iron boots)", Tailor.RemoveArmorFromOutfit(OutfitName, akBoots), True)
	ExpectBool("RemoveArmorFromOutfit(iron boots) again, the outfit hasn't got them", Tailor.RemoveArmorFromOutfit(OutfitName, akBoots), False)
	ExpectBool("RemoveArmorFromOutfit(None)", Tailor.RemoveArmorFromOutfit(OutfitName, None), False)
	ExpectBool("AddArmorToOutfit(iron boots), back again", Tailor.AddArmorToOutfit(OutfitName, akBoots), True)

	ExpectBool("AddCategory(" + OutfitName + ", " + CategoryName + ")", Tailor.AddCategory(OutfitName, CategoryName), True)
	ExpectBool("AddCategory(no such outfit, " + CategoryName + ")", Tailor.AddCategory("No Such Outfit", CategoryName), False)
	ExpectBool("AddCategory(" + OutfitName + ", no such category)", Tailor.AddCategory(OutfitName, "No Such Category"), False)
EndFunction


;  OUTFITS: DoesOutfitExist, GetOutfit, GetOutfitGender, GetOutfitArmors, OutfitHasKeyword, OutfitUsesSlot, IsOutfitInCategory

Function TestOutfits()
	Actor akPlayer = Game.GetPlayer()
	Keyword akCuirassKeyword = Game.GetForm(0x0006C0EC) as Keyword   ; Skyrim.esm: ArmorCuirass

	ExpectBool("DoesOutfitExist(" + OutfitName + ")", Tailor.DoesOutfitExist(OutfitName), True)
	ExpectBool("DoesOutfitExist(" + OutfitName + " with other capitals and spaces)", Tailor.DoesOutfitExist("  tailorapitest OUTFIT "), True)
	ExpectBool("DoesOutfitExist(No Such Outfit)", Tailor.DoesOutfitExist("No Such Outfit"), False)
	ExpectBool("DoesOutfitExist(an empty name)", Tailor.DoesOutfitExist(""), False)

	Note("GetOutfit(player)", "'" + Tailor.GetOutfit(akPlayer) + "'")
	If TestNpc
		Note("GetOutfit(TestNpc)", "'" + Tailor.GetOutfit(TestNpc) + "'")
	EndIf
	ExpectString("GetOutfit(None)", Tailor.GetOutfit(None), "")

	ExpectInt("GetOutfitGender(" + OutfitName + "), unisex", Tailor.GetOutfitGender(OutfitName), -1)
	ExpectInt("GetOutfitGender(No Such Outfit)", Tailor.GetOutfitGender("No Such Outfit"), -2)

	Armor[] akArmors = Tailor.GetOutfitArmors(OutfitName)
	ExpectInt("GetOutfitArmors(" + OutfitName + ") length", akArmors.Length, 2)
	Int iArmor = 0
	While iArmor < akArmors.Length
		Debug.Trace("[TailorApiTest]   piece " + iArmor + ": " + akArmors[iArmor].GetName())
		iArmor += 1
	EndWhile
	ExpectInt("GetOutfitArmors(No Such Outfit) length", Tailor.GetOutfitArmors("No Such Outfit").Length, 0)

	ExpectBool("OutfitHasKeyword(" + OutfitName + ", ArmorCuirass)", Tailor.OutfitHasKeyword(OutfitName, akCuirassKeyword), True)
	ExpectBool("OutfitHasKeyword(" + OutfitName + ", None)", Tailor.OutfitHasKeyword(OutfitName, None), False)
	ExpectBool("OutfitHasKeyword(No Such Outfit, ArmorCuirass)", Tailor.OutfitHasKeyword("No Such Outfit", akCuirassKeyword), False)

	ExpectBool("OutfitUsesSlot(" + OutfitName + ", 32), the body", Tailor.OutfitUsesSlot(OutfitName, 32), True)
	ExpectBool("OutfitUsesSlot(" + OutfitName + ", 37), the feet", Tailor.OutfitUsesSlot(OutfitName, 37), True)
	ExpectBool("OutfitUsesSlot(" + OutfitName + ", 31), the hair", Tailor.OutfitUsesSlot(OutfitName, 31), False)
	ExpectBool("OutfitUsesSlot(" + OutfitName + ", 62), outside 30-61", Tailor.OutfitUsesSlot(OutfitName, 62), False)

	ExpectBool("IsOutfitInCategory(" + OutfitName + ", " + CategoryName + ")", Tailor.IsOutfitInCategory(OutfitName, CategoryName), True)
	ExpectBool("IsOutfitInCategory(" + OutfitName + ", Warm)", Tailor.IsOutfitInCategory(OutfitName, "Warm"), False)
	ExpectBool("IsOutfitInCategory(No Such Outfit, " + CategoryName + ")", Tailor.IsOutfitInCategory("No Such Outfit", CategoryName), False)
EndFunction


;  LISTS: DoesCategoryExist, GetOutfitsByCategory, GetOutfitCount, GetAdventuringOutfits, GetOutfitsByArmorKeyword

Function TestLists()
	Keyword akCuirassKeyword = Game.GetForm(0x0006C0EC) as Keyword   ; Skyrim.esm: ArmorCuirass

	ExpectBool("DoesCategoryExist(" + CategoryName + ")", Tailor.DoesCategoryExist(CategoryName), True)
	ExpectBool("DoesCategoryExist(Town)", Tailor.DoesCategoryExist("Town"), True)
	ExpectBool("DoesCategoryExist(No Such Category)", Tailor.DoesCategoryExist("No Such Category"), False)

	String[] asNames = Tailor.GetOutfitsByCategory(CategoryName)
	ExpectInt("GetOutfitsByCategory(" + CategoryName + ") length", asNames.Length, 1)
	ExpectBool("GetOutfitsByCategory(" + CategoryName + ") has " + OutfitName, asNames.Find(OutfitName) >= 0, True)
	ExpectInt("GetOutfitsByCategory(" + CategoryName + ", 0, men) length", Tailor.GetOutfitsByCategory(CategoryName, 0).Length, 1)
	ExpectInt("GetOutfitsByCategory(" + CategoryName + ", 1, women) length", Tailor.GetOutfitsByCategory(CategoryName, 1).Length, 1)
	ExpectInt("GetOutfitsByCategory(" + CategoryName + ", 2) length, not a gender filter", Tailor.GetOutfitsByCategory(CategoryName, 2).Length, 0)
	ExpectInt("GetOutfitsByCategory(No Such Category) length", Tailor.GetOutfitsByCategory("No Such Category").Length, 0)

	ExpectInt("GetOutfitCount(" + CategoryName + ")", Tailor.GetOutfitCount(CategoryName), 1)
	ExpectInt("GetOutfitCount(" + CategoryName + ", 1, women)", Tailor.GetOutfitCount(CategoryName, 1), 1)
	ExpectInt("GetOutfitCount(" + CategoryName + ", 2), not a gender filter", Tailor.GetOutfitCount(CategoryName, 2), 0)
	ExpectInt("GetOutfitCount(No Such Category)", Tailor.GetOutfitCount("No Such Category"), 0)
	ExpectInt("GetOutfitCount(Town) matches GetOutfitsByCategory(Town)", Tailor.GetOutfitCount("Town"), Tailor.GetOutfitsByCategory("Town").Length)

	Note("GetAdventuringOutfits() length", Tailor.GetAdventuringOutfits().Length)
	Note("GetAdventuringOutfits(heavy) length", Tailor.GetAdventuringOutfits("heavy").Length)
	Note("GetAdventuringOutfits(clothing, 1, women) length", Tailor.GetAdventuringOutfits("clothing", 1).Length)
	ExpectInt("GetAdventuringOutfits(shiny) length, not an armor type", Tailor.GetAdventuringOutfits("shiny").Length, 0)

	ExpectBool("GetOutfitsByArmorKeyword(ArmorCuirass) has " + OutfitName, Tailor.GetOutfitsByArmorKeyword(akCuirassKeyword).Find(OutfitName) >= 0, True)
	ExpectInt("GetOutfitsByArmorKeyword(None) length", Tailor.GetOutfitsByArmorKeyword(None).Length, 0)
EndFunction


;  SITUATIONS: GetSituation, HasSituation

Function TestSituations()
	Note("GetSituation(player)", "'" + Tailor.GetSituation(Game.GetPlayer()) + "'")
	If TestNpc
		Note("GetSituation(TestNpc)", "'" + Tailor.GetSituation(TestNpc) + "'")
	EndIf
	ExpectString("GetSituation(None)", Tailor.GetSituation(None), "")
	Note("HasSituation(player, swimming)", BoolText(Tailor.HasSituation(Game.GetPlayer(), "swimming")))
	If TestNpc
		Note("HasSituation(TestNpc, swimming)", BoolText(Tailor.HasSituation(TestNpc, "swimming")))
	EndIf
	ExpectBool("HasSituation(None, swimming)", Tailor.HasSituation(None, "swimming"), False)
	ExpectBool("HasSituation(player, beach)", Tailor.HasSituation(Game.GetPlayer(), "beach"), False)
EndFunction


;  OVERRIDES: OverrideWithOutfit, OverrideWithSituation, OverrideWithCategory, HasOutfitOverride, ClearOutfitOverride

Function TestOverrides()
	TestOverridesOn(Game.GetPlayer(), "player")
	If TestNpc
		TestOverridesOn(TestNpc, "TestNpc")
	EndIf
	ExpectBool("OverrideWithOutfit(None, " + OutfitName + ")", Tailor.OverrideWithOutfit(None, OutfitName), False)
	ExpectBool("OverrideWithSituation(None, town)", Tailor.OverrideWithSituation(None, "town"), False)
	ExpectBool("OverrideWithCategory(None, " + CategoryName + ")", Tailor.OverrideWithCategory(None, CategoryName), False)
	ExpectBool("HasOutfitOverride(None)", Tailor.HasOutfitOverride(None), False)
	ExpectBool("ClearOutfitOverride(None)", Tailor.ClearOutfitOverride(None), False)
EndFunction

; An override is accepted at once and goes on from the next frame, so each check of what they wear waits first
Function TestOverridesOn(Actor akPerson, String asLabel)
	Note("ClearOutfitOverride(" + asLabel + "), to start with none", BoolText(Tailor.ClearOutfitOverride(akPerson)))
	ExpectBool("HasOutfitOverride(" + asLabel + ") with none", Tailor.HasOutfitOverride(akPerson), False)

	ExpectBool("OverrideWithOutfit(" + asLabel + ", No Such Outfit)", Tailor.OverrideWithOutfit(akPerson, "No Such Outfit"), False)
	ExpectBool("OverrideWithSituation(" + asLabel + ", shiny)", Tailor.OverrideWithSituation(akPerson, "shiny"), False)
	ExpectBool("OverrideWithCategory(" + asLabel + ", No Such Category)", Tailor.OverrideWithCategory(akPerson, "No Such Category"), False)
	ExpectBool("HasOutfitOverride(" + asLabel + ") after refusals", Tailor.HasOutfitOverride(akPerson), False)

	ExpectBool("OverrideWithOutfit(" + asLabel + ", " + OutfitName + ")", Tailor.OverrideWithOutfit(akPerson, OutfitName), True)
	ExpectBool("HasOutfitOverride(" + asLabel + ")", Tailor.HasOutfitOverride(akPerson), True)
	Utility.Wait(1.5)
	ExpectString("GetOutfit(" + asLabel + ") with the override", Tailor.GetOutfit(akPerson), OutfitName)

	; the last override set wins; the category has one outfit, so the pick is that one
	ExpectBool("OverrideWithCategory(" + asLabel + ", " + CategoryName + ")", Tailor.OverrideWithCategory(akPerson, CategoryName), True)
	Utility.Wait(1.5)
	ExpectString("GetOutfit(" + asLabel + ") with the category override", Tailor.GetOutfit(akPerson), OutfitName)

	; what this accepts depends on the person's own outfits and the pool, so it is reported, not judged
	Note("OverrideWithSituation(" + asLabel + ", town)", BoolText(Tailor.OverrideWithSituation(akPerson, "town")))
	Utility.Wait(1.5)
	Note("GetOutfit(" + asLabel + ") with the situation override", "'" + Tailor.GetOutfit(akPerson) + "'")

	ExpectBool("ClearOutfitOverride(" + asLabel + ")", Tailor.ClearOutfitOverride(akPerson), True)
	ExpectBool("ClearOutfitOverride(" + asLabel + ") again", Tailor.ClearOutfitOverride(akPerson), False)
	ExpectBool("HasOutfitOverride(" + asLabel + ") after clearing", Tailor.HasOutfitOverride(akPerson), False)
EndFunction


;  UTILITY: EvaluateOutfit

Function TestUtility()
	ExpectBool("EvaluateOutfit(player)", Tailor.EvaluateOutfit(Game.GetPlayer()), True)
	; true only for an NPC Tailor dresses, or one with an override
	If TestNpc
		Note("EvaluateOutfit(TestNpc)", BoolText(Tailor.EvaluateOutfit(TestNpc)))
	EndIf
	ExpectBool("EvaluateOutfit(None)", Tailor.EvaluateOutfit(None), False)
EndFunction


;  REPORTING

; A call whose result is known: logs it, and counts it as expected or not
Function ExpectBool(String asCall, Bool abResult, Bool abExpected)
	Judge(asCall, BoolText(abResult), BoolText(abExpected), abResult == abExpected)
EndFunction

Function ExpectInt(String asCall, Int aiResult, Int aiExpected)
	Judge(asCall, aiResult, aiExpected, aiResult == aiExpected)
EndFunction

Function ExpectString(String asCall, String asResult, String asExpected)
	Judge(asCall, "'" + asResult + "'", "'" + asExpected + "'", asResult == asExpected)
EndFunction

Function Judge(String asCall, String asResult, String asExpected, Bool abAsExpected)
	_calls += 1
	_expected += 1
	If abAsExpected
		_met += 1
		Debug.Trace("[TailorApiTest] ok     " + asCall + " = " + asResult)
	Else
		Debug.Trace("[TailorApiTest] WRONG  " + asCall + " = " + asResult + ", expected " + asExpected)
	EndIf
EndFunction

; A call whose result depends on the player's library and the person: logs it, judges nothing
Function Note(String asCall, String asResult)
	_calls += 1
	Debug.Trace("[TailorApiTest] note   " + asCall + " = " + asResult)
EndFunction

String Function BoolText(Bool abValue)
	If abValue
		Return "true"
	EndIf
	Return "false"
EndFunction

String Function NameOf(Actor akActor)
	If !akActor
		Return "(nobody)"
	ElseIf akActor == Game.GetPlayer()
		Return "player"
	EndIf
	Return akActor.GetActorBase().GetName()
EndFunction
