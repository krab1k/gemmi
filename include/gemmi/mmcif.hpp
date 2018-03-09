// Copyright 2017 Global Phasing Ltd.
//
// Read mmcif (PDBx/mmCIF) file into a Structure from model.hpp.

#ifndef GEMMI_MMCIF_HPP_
#define GEMMI_MMCIF_HPP_

#include <array>
#include <string>
#include <unordered_map>
#include "cifdoc.hpp"
#include "numb.hpp"
#include "model.hpp"

namespace gemmi {

namespace impl {

inline std::unordered_map<std::string, std::array<float,6>>
get_anisotropic_u(cif::Block& block) {
  cif::Table aniso_tab = block.find("_atom_site_anisotrop.",
                                    {"id", "U[1][1]", "U[2][2]", "U[3][3]",
                                     "U[1][2]", "U[1][3]", "U[2][3]"});
  std::unordered_map<std::string, std::array<float,6>> aniso_map;
  for (auto ani : aniso_tab)
    aniso_map[ani[0]] = {{(float) cif::as_number(ani[1]),
                          (float) cif::as_number(ani[2]),
                          (float) cif::as_number(ani[3]),
                          (float) cif::as_number(ani[4]),
                          (float) cif::as_number(ani[5]),
                          (float) cif::as_number(ani[6])}};
  return aniso_map;
}

inline
std::vector<std::string> transform_tags(std::string mstr, std::string vstr) {
  return {mstr + "[1][1]", mstr + "[1][2]", mstr + "[1][3]", vstr + "[1]",
          mstr + "[2][1]", mstr + "[2][2]", mstr + "[2][3]", vstr + "[2]",
          mstr + "[3][1]", mstr + "[3][2]", mstr + "[3][3]", vstr + "[3]"};
}

inline Transform get_transform_matrix(const cif::Table::Row& r) {
  Transform t;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j)
      t.mat[i][j] = cif::as_number(r[4*i+j]);
    t.vec.at(i) = cif::as_number(r[4*i+3]);
  }
  return t;
}

inline int parse_auth_seqid(const std::string& seqid, char& ins_code) {
  // old mmCIF files have auth_seq_id as number + icode (e.g. 15A)
  if (!seqid.empty() && seqid.back() >= 'A') {
    if (ins_code != seqid.back()) {
      if (ins_code != '\0')
        fail("Inconsistent insertion code in " + seqid);
      ins_code = seqid.back();
    }
    return cif::as_int(seqid.substr(0, seqid.size() - 1));
  }
  return cif::as_int(seqid, Residue::OptionalNum::None);
}

inline void read_connectivity(cif::Block& block, Structure& st) {
  for (auto row : block.find("_struct_conn.", {"id", "conn_type_id", // 0-1
        "ptnr1_label_asym_id", "ptnr2_label_asym_id", // 2-3
        "ptnr1_label_seq_id", "ptnr2_label_seq_id",   // 4-5
        "ptnr1_label_comp_id", "ptnr2_label_comp_id", // 6-7
        "ptnr1_label_atom_id", "ptnr2_label_atom_id", // 8-9
        "?pdbx_ptnr1_label_alt_id", "?pdbx_ptnr2_label_alt_id", // 10-11
        // the label_ atom identifiers are not sufficient for HOH:
        // waters have null as label_seq_id so the "main" identifier cannot
        // distinguish waters in the same chain. So we use "alternative"
        // identifier if available.
        "?ptnr1_auth_seq_id", "?ptnr2_auth_seq_id", // 12-13
        "?pdbx_ptnr1_PDB_ins_code", "?pdbx_ptnr2_PDB_ins_code", // 14-15
        "?ptnr1_symmetry", "?ptnr2_symmetry"/*16-17*/})) {
    Connection c;
    c.name = row.str(0);
    std::string type = row.str(1);
    for (int i = 0; i != Connection::None; ++i)
      if (get_mmcif_connection_type_id(Connection::Type(i)) == type) {
        c.type = Connection::Type(i);
        break;
      }
    if (row.has2(16) && row.has2(17)) {
      if (row.str(16) == row.str(17))
        c.image = SymmetryImage::Same;
      else
        c.image = SymmetryImage::Different;
    }
    for (int i = 0; i < 2; ++i) {
      AtomAddress& a = c.atom[i];
      a.chain_name = row.str(2+i);
      a.use_auth_name = false;
      a.res_id.label_seq = cif::as_int(row[4+i], Residue::OptionalNum::None);
      a.res_id.name = row.str(6+i);
      if (row.has(14+i))
        a.res_id.icode = row.str(14+i)[0];
      if (row.has2(12+i))
        a.res_id.seq_num = parse_auth_seqid(row[12+i], a.res_id.icode);
      a.atom_name = row.str(8+i);
      a.altloc = row.has2(10+i) ? row.str(10+i)[0] : '\0';
    }
    for (Model& mdl : st.models)
      mdl.connections.emplace_back(c);
  }
}

inline Structure structure_from_cif_block(cif::Block& block) {
  using cif::as_number;
  using cif::as_string;
  Structure st;
  st.name = block.name;

  // unit cell and symmetry
  cif::Table cell = block.find("_cell.",
                               {"length_a", "length_b", "length_c",
                                "angle_alpha", "angle_beta", "angle_gamma"});
  if (cell.ok()) {
    auto c = cell.one();
    if (!cif::is_null(c[0]) && !cif::is_null(c[1]) && !cif::is_null(c[2]))
      st.cell.set(as_number(c[0]), as_number(c[1]), as_number(c[2]),
                  as_number(c[3]), as_number(c[4]), as_number(c[5]));
  }
  st.sg_hm = as_string(block.find_value("_symmetry.space_group_name_H-M"));

  auto add_info = [&](std::string tag) {
    cif::Column col = block.find_values(tag);
    bool first = true;
    for (const std::string& v : col)
      if (!cif::is_null(v)) {
        if (first)
          st.info[tag] = as_string(v);
        else
          st.info[tag] += "; " + as_string(v);
        first = false;
      }
  };
  add_info("_entry.id");
  add_info("_cell.Z_PDB");
  add_info("_exptl.method");
  add_info("_struct.title");
  // in pdbx/mmcif v5 date_original was replaced with a much longer tag
  std::string old_date_tag = "_database_PDB_rev.date_original";
  std::string new_date_tag
                      = "_pdbx_database_status.recvd_initial_deposition_date";
  add_info(old_date_tag);
  add_info(new_date_tag);
  if (st.info.count(old_date_tag) == 1 && st.info.count(new_date_tag) == 0)
    st.info[new_date_tag] = st.info[old_date_tag];
  add_info("_struct_keywords.pdbx_keywords");
  add_info("_struct_keywords.text");

  for (const std::string& d : block.find_values("_refine.ls_d_res_high")) {
    double resol = cif::as_number(d);
    if (resol > 0 && (st.resolution == 0 || resol < st.resolution))
      st.resolution = resol;
  }

  std::vector<std::string> ncs_oper_tags = transform_tags("matrix", "vector");
  ncs_oper_tags.emplace_back("id");  // 12
  ncs_oper_tags.emplace_back("?code");  // 13
  cif::Table ncs_oper = block.find("_struct_ncs_oper.", ncs_oper_tags);
  for (auto op : ncs_oper) {
    bool given = op.has(13) && op.str(13) == "given";
    Transform tr = get_transform_matrix(op);
    if (!tr.is_identity())
      st.ncs.push_back({op.str(12), given, tr});
  }

  // PDBx/mmcif spec defines both _database_PDB_matrix.scale* and
  // _atom_sites.fract_transf_* as equivalent of pdb SCALE, but the former
  // is not used, so we ignore it.
  cif::Table fract_tv = block.find("_atom_sites.fract_transf_",
                                   transform_tags("matrix", "vector"));
  if (fract_tv.length() > 0) {
    Transform fract = get_transform_matrix(fract_tv[0]);
    st.cell.set_matrices_from_fract(fract);
  }

  // We read/write origx just for completeness, it's not used anywhere.
  cif::Table origx_tv = block.find("_database_PDB_matrix.",
                                   transform_tags("origx", "origx_vector"));
  if (origx_tv.length() > 0)
    st.origx = get_transform_matrix(origx_tv[0]);

  auto aniso_map = get_anisotropic_u(block);

  // atom list
  enum { kId=0, kSymbol, kAtomId, kAltId, kCompId, kAsymId, kSeqId, kInsCode,
         kX, kY, kZ, kOcc, kBiso, kCharge, kAuthSeqId, kAuthAsymId, kModelNum };
  cif::Table atom_table = block.find("_atom_site.",
                                     {"id",
                                      "type_symbol",
                                      "label_atom_id",
                                      "label_alt_id",
                                      "label_comp_id",
                                      "label_asym_id",
                                      "label_seq_id",
                                      "pdbx_PDB_ins_code",
                                      "Cartn_x",
                                      "Cartn_y",
                                      "Cartn_z",
                                      "occupancy",
                                      "B_iso_or_equiv",
                                      "pdbx_formal_charge",
                                      "auth_seq_id",
                                      "auth_asym_id",
                                      "pdbx_PDB_model_num"});
  Model *model = nullptr;
  Chain *chain = nullptr;
  Residue *resi = nullptr;
  for (auto row : atom_table) {
    if (!model || row[kModelNum] != model->name) {
      model = &st.find_or_add_model(row[kModelNum]);
      chain = nullptr;
    }
    if (!chain || row[kAsymId] != chain->name) {
      chain = &model->find_or_add_chain(row.str(kAsymId));
      chain->auth_name = row.str(kAuthAsymId);
      resi = nullptr;
    }
    ResidueId rid;
    rid.label_seq = cif::as_int(row[kSeqId], Residue::OptionalNum::None);
    rid.icode = as_string(row[kInsCode])[0];
    rid.seq_num = parse_auth_seqid(row[kAuthSeqId], rid.icode);
    rid.name = as_string(row[kCompId]);
    if (!resi || !resi->matches(rid)) {
      // the insertion code happens to be always a single letter
      assert(row[kInsCode].size() == 1);
      resi = chain->find_or_add_residue(rid);
    } else {
      assert(resi->seq_num == rid.seq_num);
      assert(resi->icode == rid.icode);
    }
    Atom atom;
    atom.name = as_string(row[kAtomId]);
    atom.altloc = as_string(row[kAltId])[0];
    atom.charge = cif::is_null(row[kCharge]) ? 0 : cif::as_int(row[kCharge]);
    atom.element = gemmi::Element(as_string(row[kSymbol]));
    atom.pos.x = cif::as_number(row[kX]);
    atom.pos.y = cif::as_number(row[kY]);
    atom.pos.z = cif::as_number(row[kZ]);
    atom.occ = (float) cif::as_number(row[kOcc], 1.0);
    atom.b_iso = (float) cif::as_number(row[kBiso], 50.0);

    if (!aniso_map.empty()) {
      auto ani = aniso_map.find(row[kId]);
      if (ani != aniso_map.end()) {
        atom.u11 = ani->second[0];
        atom.u22 = ani->second[1];
        atom.u33 = ani->second[2];
        atom.u12 = ani->second[3];
        atom.u13 = ani->second[4];
        atom.u23 = ani->second[5];
      }
    }
    resi->atoms.emplace_back(atom);
  }

  cif::Table polymer_types = block.find("_entity_poly.", {"entity_id", "type"});
  for (auto row : block.find("_entity.", {"id", "type"})) {
    std::string id = row.str(0);
    Entity ent;
    ent.entity_type = entity_type_from_string(row.str(1));
    ent.polymer_type = PolymerType::Unknown;
    if (polymer_types.ok()) {
      try {
        std::string poly_type = polymer_types.find_row(id).str(1);
        ent.polymer_type = polymer_type_from_string(poly_type);
      } catch (std::runtime_error&) {}
    }
    st.entities.emplace(id, ent);
  }

  for (auto row : block.find("_entity_poly_seq.",
                             {"entity_id", "num", "mon_id"})) {
    Entity& ent = st.find_or_add_entity(row.str(0));
    ent.sequence.push_back({cif::as_int(row[1], -1), row.str(2)});
  }

  auto chain_to_entity = block.find("_struct_asym.", {"id", "entity_id"});
  for (Model& mod : st.models)
    for (Chain& ch : mod.chains) {
      try {
        ch.entity_id = chain_to_entity.find_row(ch.name).str(1);
      } catch (std::runtime_error&) {
        // maybe _struct_asym is missing
      }
    }
  st.setup_cell_images();

  // CISPEP
  for (auto row : block.find("_struct_mon_prot_cis.",
                             {"pdbx_PDB_model_num", "label_asym_id",
                              "label_seq_id", "label_comp_id"})) {
    if (row.has2(0) && row.has2(1) && row.has2(2) && row.has2(3))
      if (Model* mdl = st.find_model(row[0])) {
        ResidueId rid;
        rid.label_seq = cif::as_int(row[2]);
        rid.name = row.str(3);
        if (Chain* ch = mdl->find_chain(row[1]))
          if (Residue* res = ch->find_residue(rid))
            res->is_cis = true;
      }
  }

  read_connectivity(block, st);

  return st;
}

} // namespace impl

inline Structure make_structure_from_block(cif::Block& block) {
  return impl::structure_from_cif_block(block);
}

// the name of this function may change
inline Structure read_atoms(cif::Document doc) {
  return impl::structure_from_cif_block(doc.sole_block());
}

} // namespace gemmi
#endif
// vim:sw=2:ts=2:et
