;/* TailorApiTestPlayerAlias
* * the other half of TailorApiTest (see its header for how to set both up): a script for a Reference Alias
* * on the same quest, filled with the player. SKSE forgets event registrations at every load and a quest
* * script has no OnPlayerLoadGame, so this makes the test listen to Tailor's events again after each load
*/;
Scriptname TailorApiTestPlayerAlias extends ReferenceAlias

Event OnPlayerLoadGame()
	TailorApiTest akTest = GetOwningQuest() as TailorApiTest
	If akTest
		akTest.RegisterEvents()
	Else
		Debug.Trace("[TailorApiTest] TailorApiTestPlayerAlias is not on a quest that has TailorApiTest")
	EndIf
EndEvent
