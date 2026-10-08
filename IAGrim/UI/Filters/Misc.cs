using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using IAGrim.Services.ItemStats;

namespace IAGrim.UI.Filters {
    public partial class Misc : UserControl {
        private FirefoxCheckBox? cbMonsterInfrequents;
        private FirefoxCheckBox? cbExcludeMonsterInfrequents;
        public Misc() {
            InitializeComponent();
            cbMonsterInfrequents = CreateMiCheckBox("Monster Infrequents only", 32);
            cbExcludeMonsterInfrequents = CreateMiCheckBox("Exclude Monster Infrequents", 59);
            cbMonsterInfrequents.Width = 285;
            cbExcludeMonsterInfrequents.Width = 285;

            // Keep every original Misc control explicitly represented. The custom panel's designer
            // collection can omit controls when it is reparented/scaled, so do not infer this list.
            var existing = new Control[] {
                exp, health, cbDefense, cbOffensive, cbRunspeed, cbCastspeed, cbAttackSpeed,
                cbMasterySkills, cbPetBonuses, cbHasPetBonus, setbonus, shieldStuff, cbReflect,
                cbDuplicates, cbSocketed, cbRecentOnly, cbGrantsSkill, cbSummonerSkill,
                cbEnergyRegen, cbWeaponLifeLeech, cbDamageConversion, cbCooldownReduction,
                cbIncreaseArmor, cbPhysique, cbSpirit, cbCunning,
            };

            for (var i = 0; i < existing.Length; i++) {
                var row = i / 2;
                var column = i % 2;
                existing[i].Left = column == 0 ? 3 : 148;
                existing[i].Top = 92 + row * 30;
                existing[i].Width = 140;
            }
            miscPanel.Height = Math.Max(miscPanel.Height, 92 + ((existing.Length + 1) / 2) * 30 + 8);
            health.SupportsNumericFilter = true;
            cbDefense.SupportsNumericFilter = true;
            cbOffensive.SupportsNumericFilter = true;
        }

        public void Misc_Load(object sender, EventArgs e) {
            miscPanel.ToggleState();
        }

        // The three Misc stats that expose a numeric filter button, paired with the same fields their
        // "stat exists" entries use in Filters below.
        public List<StatValueFilter> NumericFilters => FilterBuilder.From(new (FirefoxCheckBox, string[])[] {
            (health, new[] { "characterLifeModifier", "characterLife" }),
            (cbDefense, new[] { "characterDefensiveAbilityModifier", "characterDefensiveAbility" }),
            (cbOffensive, new[] { "characterOffensiveAbility", "characterOffensiveAbilityModifier" }),
        });

        public bool SocketedOnly => cbSocketed.Checked;
        public bool DuplicatesOnly => cbDuplicates.Checked;
        public bool PetBonuses => cbPetBonuses.Checked;
        public bool HasPetBonus => cbHasPetBonus.Checked;
        public bool RecentOnly => cbRecentOnly.Checked;
        public bool GrantsSkill => cbGrantsSkill.Checked;
        public bool WithSummonerSkillOnly => cbSummonerSkill.Checked;
        public bool MonsterInfrequentOnly => cbMonsterInfrequents?.Checked == true;
        public bool ExcludeMonsterInfrequents => cbExcludeMonsterInfrequents?.Checked == true;

        private FirefoxCheckBox CreateMiCheckBox(string text, int y) {
            var cb = new FirefoxCheckBox {
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right,
                AutoSize = false,
                Bold = false,
                EnabledCalc = true,
                Font = new Font("Segoe UI", 10F),
                Location = new Point(3, y),
                Size = new Size(272, 27),
                Text = text,
                UseVisualStyleBackColor = true,
            };
            miscPanel.Controls.Add(cb);
            return cb;
        }


        public List<string[]> Filters {
            get {
                var filters = new List<string[]>();

                if (setbonus.Checked) {
                    filters.Add(new[] { "setName", "itemSetName" });
                }

                if (shieldStuff.Checked) {
                    filters.Add(new[] {
                        "blockAbsorption", "defensiveBlock", "defensiveBlockChance", "defensiveBlockModifier",
                        "defensiveBlockAmountModifier"
                    });
                }

                if (cbAttackSpeed.Checked) {
                    filters.Add(new[]
                        {"characterAttackSpeedModifier", "characterAttackSpeed", "characterTotalSpeedModifier"});
                }

                if (cbCastspeed.Checked) {
                    filters.Add(new[] {"characterSpellCastSpeedModifier", "characterTotalSpeedModifier"});
                }

                if (cbIncreaseArmor.Checked) {
                    filters.Add(new[] { "defensiveProtectionModifier" });
                }

                if (cbRunspeed.Checked) {
                    filters.Add(new[] {"characterRunSpeedModifier", "characterTotalSpeedModifier"});
                }

                if (exp.Checked) {
                    filters.Add(new[] {"characterIncreasedExperience"});
                }

                if (cbReflect.Checked) {
                    filters.Add(new[] {"defensiveReflect"});
                }

                if (health.Checked) {
                    filters.Add(new[] {"characterLifeModifier", "characterLife"});
                }

                if (cbDefense.Checked) {
                    filters.Add(new[] {"characterDefensiveAbilityModifier", "characterDefensiveAbility"});
                }

                if (cbOffensive.Checked) {
                    filters.Add(new[] {"characterOffensiveAbility", "characterOffensiveAbilityModifier"});
                }

                if (cbMasterySkills.Checked) {
                    filters.Add(new[] {"augmentMastery1", "augmentMastery2"});
                }


                if (cbEnergyRegen.Checked) {
                    filters.Add(new[] {"characterManaRegen", "characterManaRegenModifier"});
                }

                if (cbWeaponLifeLeech.Checked) {
                    filters.Add(new[] { "offensiveLifeLeechMin" });
                }

                if (cbDamageConversion.Checked) {
                    filters.Add(new[] { "conversionPercentage" });
                }

                if (cbCooldownReduction.Checked) {
                    filters.Add(new[] { "skillCooldownReduction" });
                }

                if (cbPhysique.Checked) {
                    filters.Add(new[] { "characterStrength", "characterStrengthModifier" });
                }

                if (cbSpirit.Checked) {
                    filters.Add(new[] { "characterIntelligence", "characterIntelligenceModifier" });
                }

                if (cbCunning.Checked) {
                    filters.Add(new[] { "characterDexterity", "characterDexterityModifier" });
                }

                return filters;
            }
        }
    }
}
