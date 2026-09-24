Problem Memo – A B Lopez Alvarado, Seth Glasser
GitHub: https://github.com/SethDoj/CS370-Term-Project
The user
Someone who's killed at least one orchid already, or is afraid of doing so — a friend, family member, or even one of you. For example, "my partner's mom, who's killed three phalaenopsis in two years and waters on a fixed Sunday schedule regardless of conditions".
The problem
Orchids don't fail on a fixed schedule — they fail when watering doesn't match actual drying conditions. A phalaenopsis in a warm, dry room dries out in 4 days; the same plant in a humid room takes 10. Calendar watering (the thing almost everyone actually does) either drowns the roots or starves them, and root rot is invisible until the leaves are already yellowing — weeks after the damage was done.
Why a device (the 3 a.m. test)
The rot happens silently, over days, while nobody's watching. Nobody checks their orchid's root zone every morning with a moisture meter — that's the whole reason calendar-watering exists as a coping mechanism. A device that watches continuously and says "don't water yet, still moist, conditions are humid" or "water today, it's dried out fast this week" replaces guesswork with an actual read of the plant's real situation.
The sensors (must cooperate, not just coexist)
Capacitive soil moisture sensor — TCL555I
Temperature/humidity sensor – SHT40
Light level sensor (optional, above and beyond) – VEML7700 if desired
These sensors will all work in unison to generate the data necessary to determine if the plant is in a healthy system or not. Too hot, not humid enough, too much water in the soil, etc…
Mechanisms
Menu Item E: Multi-Process architecture – Even if one sensor goes down, we want to have data from the other. 
Menu Item D: Crash-consistent storage, recovery after crash – If we crash in the middle of a write, we want to be able to restore to a steady state, not just hope all data is corrupted.

Risk
The most likely thing to go wrong is that a sensor goes bad and we do not have backups. Luckily, most sensors ship in multiple units, so we ideally should always have a backup.
