// Fuchey companion — marketplace config. Copy this file to config.local.js
// (gitignored) and fill in the values from the fuchey.xyz Supabase project
// (Project settings → API). Both are public: the anon key can only read what
// the database's RLS allows (published wearables and saved looks).
// Never put the service-role key here.
export default {
  SUPABASE_URL: "https://<project>.supabase.co",
  SUPABASE_ANON_KEY: "<anon key>",
};
