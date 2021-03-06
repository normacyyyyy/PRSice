// This file is part of PRSice2.0, copyright (C) 2016-2017
// Shing Wan Choi, Jack Euesden, Cathryn M. Lewis, Paul F. Oâ€™Reilly
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "prsice.hpp"

std::mutex PRSice::lock_guard;
void PRSice::pheno_check(const Commander& c_commander, Reporter& reporter)
{
    std::vector<std::string> pheno_header = c_commander.pheno_col();
    std::string pheno_file = c_commander.pheno_file();
    std::string message = "";
    if (pheno_file.empty()) {
        pheno_info.use_pheno = false;
        pheno_info.binary.push_back(c_commander.is_binary(0));
    }
    else
    {
        std::ifstream pheno;
        pheno.open(pheno_file.c_str());
        if (!pheno.is_open()) {
            std::string error_message =
                "Cannot open phenotype file: " + pheno_file;
            throw std::runtime_error(error_message);
        }
        std::string line;
        std::getline(pheno, line); // assume header line
        if (line.empty()) {
            throw std::runtime_error(
                "Cannot have empty header line for phenotype file!");
        }
        pheno.close();
        misc::trim(line);
        std::vector<std::string> col = misc::split(line);
        if (col.size() < (size_t)(2 + !m_ignore_fid)) {
            throw std::runtime_error(
                "Error: Not enough column in Phenotype file. "
                "Have you use the --ignore-fid option");
        }
        std::string sample_id = col[0];
        if (!m_ignore_fid && col.size() > 1) sample_id.append("+" + col[1]);
        message.append("Check Phenotype file: " + pheno_file + "\n");
        message.append("Column Name of Sample ID: " + sample_id + "\n");
        message.append("Note: If the phenotype file does not contain a header, "
                       "the column name will be displayed as the Sample ID "
                       "which is ok.\n");
        bool found = false;
        std::unordered_map<std::string, bool> dup_col;
        if (pheno_header.size() == 0) {
            pheno_info.use_pheno = true;
            pheno_info.col.push_back(1 + !m_ignore_fid);

            pheno_info.name.push_back("Phenotype");
            pheno_info.order.push_back(0);
            pheno_info.binary.push_back(c_commander.is_binary(0));
            message.append("Phenotype Name: " + col[pheno_info.col.back()]
                           + "\n");
        }
        else
        {
            for (size_t i_pheno = 0; i_pheno < pheno_header.size(); ++i_pheno) {
                if (dup_col.find(pheno_header[i_pheno]) == dup_col.end()) {
                    found = false;
                    dup_col[pheno_header[i_pheno]] = true;
                    // start from 1+!m_ignore_fid to skip the iid and fid part
                    for (size_t i_column = 1 + !m_ignore_fid;
                         i_column < col.size(); ++i_column)
                    {
                        if (col[i_column].compare(pheno_header[i_pheno]) == 0) {
                            found = true;
                            pheno_info.use_pheno = true;
                            pheno_info.col.push_back(i_column);
                            pheno_info.name.push_back(pheno_header[i_pheno]);
                            pheno_info.order.push_back(i_pheno);
                            pheno_info.binary.push_back(
                                c_commander.is_binary(i_pheno));
                            break;
                        }
                    }
                    if (!found) {
                        message.append(
                            "Phenotype: " + pheno_header[i_pheno]
                            + " cannot be found in phenotype file\n");
                    }
                }
            }
        }
    }
    // TODO: Might want to error out when duplicated column is detected within
    // the phenotype file
    size_t num_pheno = (pheno_info.use_pheno) ? pheno_info.col.size() : 1;
    message.append("There are a total of " + std::to_string(num_pheno)
                   + " phenotype to process\n");
    reporter.report(message);
}

void PRSice::init_matrix(const Commander& c_commander, const size_t pheno_index,
                         Genotype& target, Reporter& reporter,
                         const bool prslice)
{
    m_null_r2 = 0.0;
    m_phenotype = Eigen::VectorXd::Zero(0);
    m_independent_variables.resize(0, 0);
    m_sample_with_phenotypes.clear();
    m_null_store.clear();

    const bool no_regress = c_commander.no_regress();
    const std::string pheno_file = c_commander.pheno_file();
    const std::string output_name = c_commander.out();

    // this reset the in_regression flag of all samples
    target.reset_sample_pheno();
    // this includes all samples

    if (!no_regress) {
        gen_pheno_vec(target, pheno_file, pheno_index, !no_regress, reporter);

        gen_cov_matrix(c_commander.get_cov_file(), c_commander.get_cov_name(),
                       c_commander.get_cov_index(),
                       c_commander.get_factor_cov_index(), reporter);
    }
    // NOTE: After gen_cov_matrix, the has_pheno flag in m_sample_names is no
    // longer correct

    // now inform PRSice which samples should be included
    update_sample_included(target);

    // get the null r2
    double null_r2_adjust = 0.0;
    int n_thread = c_commander.thread();
    if (m_independent_variables.cols() > 2 && !no_regress) {
        assert(m_independent_variables.rows() == m_phenotype.rows());
        if (c_commander.is_binary(pheno_index)) {
            // ignore the first column
            // this is ok as both the first column (intercept) and the
            // second column (PRS) is currently 1
            Regression::glm(m_phenotype,
                            m_independent_variables.topRightCorner(
                                m_independent_variables.rows(),
                                m_independent_variables.cols() - 1),
                            m_null_p, m_null_r2, m_null_coeff, m_null_se, 25,
                            n_thread, true);
        }
        else
        {
            // ignore the first column
            Regression::linear_regression(
                m_phenotype,
                m_independent_variables.topRightCorner(
                    m_independent_variables.rows(),
                    m_independent_variables.cols() - 1),
                m_null_p, m_null_r2, null_r2_adjust, m_null_coeff, m_null_se,
                n_thread, true);
        }
    }
}

void PRSice::update_sample_included(Genotype& target)
{
    m_max_fid_length = 3;
    m_max_iid_length = 3;
    // anyone that's included in the study are considered
    // therefore, it should work even for multiple different
    // phenotypes
    m_matrix_index.clear();
    for (size_t i_sample = 0; i_sample < target.num_sample(); ++i_sample) {
        m_max_fid_length = (m_max_fid_length > target.fid(i_sample).length())
                               ? m_max_fid_length
                               : target.fid(i_sample).length();
        m_max_iid_length = (m_max_iid_length > target.iid(i_sample).length())
                               ? m_max_iid_length
                               : target.iid(i_sample).length();

        // update the in regression flag according to covariate

        if (m_sample_with_phenotypes.find(target.sample_id(i_sample))
            != m_sample_with_phenotypes.end())
        {
            m_matrix_index.push_back(i_sample);
            target.set_in_regression(i_sample);
        }
    }
}

void PRSice::gen_pheno_vec(Genotype& target, const std::string& pheno_file_name,
                           const int pheno_index, bool regress,
                           Reporter& reporter)
{
    std::vector<double> pheno_store;
    // reserve the maximum size (All samples)
    pheno_store.reserve(target.num_sample());
    const bool binary = pheno_info.binary[pheno_index];
    int max_num = 0;
    int num_case = 0;
    int num_control = 0;
    size_t invalid_pheno = 0;
    size_t num_not_found = 0;
    std::string line;
    size_t sample_index_ct = 0;
    size_t sample_ct = target.num_sample();
    std::unordered_set<double> input_sanity_check; // check if input is sensible
    std::string pheno_name = "Phenotype";
    std::string id;
    if (pheno_info.use_pheno) // use phenotype file
    {
        int pheno_col_index =
            pheno_info.col[pheno_index]; // obtain the phenotype index
        pheno_name = pheno_info.name[pheno_index];
        std::ifstream pheno_file;
        pheno_file.open(pheno_file_name.c_str());
        if (!pheno_file.is_open()) {
            std::string error_message =
                "Cannot open phenotype file: " + pheno_file_name;
            throw std::runtime_error(error_message);
        }

        // Read in phenotype from phenotype file
        std::unordered_map<std::string, std::string> phenotype_info;
        std::vector<std::string> token;
        // do not remove header line as that won't match anyway
        while (std::getline(pheno_file, line)) {
            misc::trim(line);
            if (line.empty()) continue;
            token = misc::split(line);
            if (token.size()
                <= (size_t)(pheno_index + 1
                            + !m_ignore_fid)) // need to check the range
            {
                std::string error_message =
                    "Malformed pheno file, should contain at least "
                    + std::to_string(pheno_index + 2 + !m_ignore_fid)
                    + " columns. "
                      "Have you use the --ignore-fid option?";
                throw std::runtime_error(error_message);
            }
            id = (m_ignore_fid) ? token[0] : token[0] + "_" + token[1];
            phenotype_info[id] = token[pheno_col_index];
        }
        pheno_file.close();
        for (size_t i_sample = 0; i_sample < sample_ct; ++i_sample) {
            id = target.sample_id(i_sample);
            if (phenotype_info.find(id) != phenotype_info.end()
                && phenotype_info[id].compare("NA") != 0)
            {
                try
                {
                    if (binary) {
                        int temp = misc::convert<int>(phenotype_info[id]);
                        if (temp >= 0 && temp <= 2) {
                            pheno_store.push_back(temp);
                            max_num = (temp > max_num) ? temp : max_num;
                            num_case += (temp == 1);
                            num_control += (temp == 0);
                        }
                        else
                        {
                            // so that it will add invalid
                            throw std::runtime_error(
                                "Invalid binary phenotype format!");
                        }
                    }
                    else
                    {
                        pheno_store.push_back(
                            misc::convert<double>(phenotype_info[id]));
                        if (input_sanity_check.size() < 2) {
                            input_sanity_check.insert(pheno_store.back());
                        }
                    }
                    m_sample_with_phenotypes[id] = sample_index_ct++;
                }
                catch (const std::runtime_error& error)
                {
                    invalid_pheno++;
                }
            }
            else
            {
                num_not_found++;
            }
        }
    }
    else
    {
        // No phenotype file is provided
        // Use information from the fam file directly
        for (size_t i_sample = 0; i_sample < sample_ct; ++i_sample) {
            if (target.pheno_is_na(i_sample)) {
                // it is ok to skip NA as default = sample.has_pheno = false
                continue;
            }
            try
            {
                if (binary) {
                    int temp = misc::convert<int>(target.pheno(i_sample));
                    if (temp >= 0 && temp <= 2) {
                        pheno_store.push_back(temp);
                        max_num = (temp > max_num) ? temp : max_num;
                        num_case += (temp == 1);
                        num_control += (temp == 0);
                    }
                    else
                    {
                        throw std::runtime_error(
                            "Invalid binary phenotype format!");
                    }
                }
                else
                {
                    pheno_store.push_back(
                        misc::convert<double>(target.pheno(i_sample)));
                    if (input_sanity_check.size() < 2) {
                        input_sanity_check.insert(pheno_store.back());
                    }
                }
                m_sample_with_phenotypes[target.sample_id(i_sample)] =
                    sample_index_ct++;
            }
            catch (const std::runtime_error& error)
            {
                invalid_pheno++;
            }
        }
    }

    std::string message = "";
    message = pheno_name + " is a ";
    if (binary) {
        message.append("binary phenotype\n");
    }
    else
    {
        message.append("continuous phenotype\n");
    }
    if (num_not_found != 0) {
        message.append(std::to_string(num_not_found)
                       + " sample(s) without phenotype\n");
    }
    if (invalid_pheno != 0) {
        message.append(std::to_string(invalid_pheno)
                       + " sample(s) with invalid phenotype\n");
    }
    if (num_not_found == sample_ct && regress) {
        message.append(
            "None of the target samples were found in the phenotype file. ");
        if (m_ignore_fid) {
            message.append(
                "Maybe the first column of your phenotype file is the FID?");
        }
        else
        {
            message.append(
                "Maybe your phenotype file doesn not contain the FID?\n");
            message.append("Might want to consider using --ignore-fid\n");
        }
        reporter.report(message);
        throw std::runtime_error("Error: No sample left");
    }
    if (invalid_pheno == sample_ct && regress) {
        message.append("Error: All sample has invalid phenotypes!");
        reporter.report(message);
        throw std::runtime_error("Error: No sample left");
    }
    if (input_sanity_check.size() < 2 && !binary && regress) {
        message.append("Only one phenotype value detected");
        auto itr = input_sanity_check.begin();
        if ((*itr) == -9) {
            message.append(" and they are all -9");
        }
        reporter.report(message);
        throw std::runtime_error("Not enough valid phenotype");
    }
    bool error = false;
    if (max_num > 1 && binary) {
        num_case = 0;
        num_control = 0;
        size_t check = 0;
        for (auto&& pheno : pheno_store) {
            pheno--;
            if (pheno < 0) {
                error = true;
            }
            else
                (pheno == 1) ? num_case++ : num_control++;
            check++;
        }
    }
    if (error && regress) {
        reporter.report(message);
        throw std::runtime_error(
            "Mixed encoding! Both 0/1 and 1/2 encoding found!");
    }
    if (pheno_store.size() == 0 && regress) {
        reporter.report(message);
        throw std::runtime_error("No phenotype presented");
    }
    // now store the vector into the m_phenotype vector
    m_phenotype =
        Eigen::Map<Eigen::VectorXd>(pheno_store.data(), pheno_store.size());


    if (binary) {
        message.append(std::to_string(num_control) + " control(s)\n");
        message.append(std::to_string(num_case) + " case(s)\n");
        if (regress) {
            if (num_control == 0)
                throw std::runtime_error("There are no control samples");
            if (num_case == 0) throw std::runtime_error("There are no cases");
        }
    }
    else
    {
        message.append(std::to_string(m_phenotype.rows())
                       + " sample(s) with valid phenotype\n");
    }
    reporter.report(message);
}

// Funcion to get the factors from the covariate file
// This function won't go live until we have good way to handle the factors
// e.g. determining the base factor & detecting if covariate is actually a
// factor
void PRSice::check_factor_cov(
    const std::string& c_cov_file, const std::vector<std::string>& c_cov_header,
    const std::vector<size_t>& cov_index,
    std::vector<std::unordered_map<std::string, int>>& factor_levels)
{
    std::ifstream cov;
    cov.open(c_cov_file.c_str());
    if (!cov.is_open()) {
        std::string error_message =
            "Error: Cannot open covariate file: " + c_cov_file;
        throw std::runtime_error(error_message);
    }
    std::string line;
    std::getline(cov, line); // remove header
    std::vector<std::unordered_map<std::string, int>> current_factors(
        cov_index.size());
    std::vector<size_t> convertable(cov_index.size(), 0);
    size_t max_index = cov_index.back() + 1;
    while (std::getline(cov, line)) {
        misc::trim(line);
        if (line.empty()) continue;
        std::vector<std::string> token = misc::split(line);
        if (token.size() < max_index) {
            std::string error_message =
                "Error: Malformed covariate file, should contain at least "
                + std::to_string(max_index) + " column!";
            throw std::runtime_error(error_message);
        }
        std::string id = (m_ignore_fid) ? token[0] : token[0] + "_" + token[1];
        if (m_sample_with_phenotypes.find(id) != m_sample_with_phenotypes.end())
        { // sample is found in the phenotype vector
            for (size_t i_cov = 0; i_cov < cov_index.size(); ++i_cov) {
                size_t covar_index = cov_index[i_cov];
                if (current_factors[i_cov].find(token[covar_index])
                    != current_factors[i_cov].end())
                {
                    current_factors[i_cov][token[covar_index]]++;
                }
                else
                {
                    current_factors[i_cov][token[covar_index]] = 1;
                }
                try
                {
                    // this is for catching unconvertable covariate
                    misc::convert<double>(token[covar_index]);
                    convertable[i_cov]++;
                }
                catch (const std::runtime_error& error)
                {
                    std::string str = token[covar_index];
                    std::transform(str.begin(), str.end(), str.begin(),
                                   ::toupper);
                    // we also consider missing as convertable
                    if (str.compare("NA") == 0 || str.compare("NULL") == 0)
                        convertable[i_cov]++;
                }
            }
        }
    }
    cov.close();
    factor_levels.resize(cov_index.size()); // make sure size is ok
    size_t num_sample = m_sample_with_phenotypes.size();
    std::ofstream log_file_stream;
    log_file_stream.open(m_log_file.c_str(), std::ofstream::app);
    if (!log_file_stream.is_open()) {
        std::string error_message =
            "Error: Cannot open log file: " + m_log_file;
        throw std::runtime_error(error_message);
    }

    for (size_t i_cov = 0; i_cov < cov_index.size(); ++i_cov) {
        // if all convertable,then it is not a factor
        if (convertable[i_cov] == num_sample) continue;
        factor_levels[i_cov] = current_factors[i_cov];
        log_file_stream << c_cov_header[cov_index[i_cov]]
                        << " is a factor with " << factor_levels[i_cov].size()
                        << " levels"
                        << "\n";
    }
    log_file_stream << "\n";
    log_file_stream.close();
}


void PRSice::process_cov_file(
    const std::string& cov_file, std::vector<uint32_t>& factor_cov_index,
    std::vector<uint32_t>& cov_start_index, std::vector<uint32_t>& cov_index,
    std::vector<std::string>& cov_name,
    std::vector<std::unordered_map<std::string, uint32_t>>& factor_levels,
    uint32_t& num_column, Reporter& reporter)
{
    // first, go through the covariate and generate the factor level vector for

    std::ifstream cov;
    std::vector<std::pair<std::string, size_t>> valid_sample_index;
    std::vector<std::string> token;
    std::vector<uint32_t> current_factor_level(factor_cov_index.size(), 0);
    std::vector<uint32_t> missing_count(cov_index.back() + 1, 0);
    std::string line, id;
    const size_t max_index = cov_index.back() + 1;
    int factor_level_index = 0;
    int num_valid = 0, index = 0;
    bool valid = true;
    factor_levels.resize(factor_cov_index.size());
    cov.open(cov_file.c_str());
    if (!cov.is_open()) {
        throw std::runtime_error("Error: Cannot open covariate file: "
                                 + cov_file);
    }
    size_t num_factors = factor_cov_index.size();
    while (std::getline(cov, line)) {
        misc::trim(line);
        if (line.empty()) continue;
        // we don't need to remove header as we will use the FID/IID to map
        // the samples and unless there's a sample called FID or IID, we should
        // be ok
        token = misc::split(line);
        if (token.size() < max_index) {
            throw std::runtime_error(
                "Error: Malformed covariate file, should have at least "
                + std::to_string(max_index) + " columns");
        }
        // check if this sample has a valid phenotype
        id = (m_ignore_fid) ? token[0] : token[0] + "_" + token[1];
        if (m_sample_with_phenotypes.find(id) != m_sample_with_phenotypes.end())
        {
            // next, check if any of the phenotype is NA

            valid = true;
            factor_level_index = 0;
            for (auto&& header : cov_index) {
                if (token[header] == "NA" || token[header] == "Na"
                    || token[header] == "nA" || token[header] == "na")
                {
                    // invalid sample
                    valid = false;
                    ++missing_count[header];
                }
                else if (factor_level_index >= num_factors
                         || header != factor_cov_index[factor_level_index])
                {
                    // check if this is covertable
                    try
                    {
                        misc::convert<double>(token[header]);
                    }
                    catch (const std::runtime_error& error)
                    {
                        valid = false;
                        ++missing_count[header];
                    }
                }
                factor_level_index +=
                    (header == factor_cov_index[factor_level_index]);
            }
            // only do the factor level thing if this
            // sample is valid
            if (valid) {
                index = m_sample_with_phenotypes[id];
                factor_level_index = 0;
                ++num_valid;
                valid_sample_index.push_back(
                    std::pair<std::string, size_t>(id, index));
                for (auto&& factor : factor_cov_index) {
                    auto&& cur_level = factor_levels[factor_level_index];
                    if (cur_level.find(token[factor]) == cur_level.end()) {
                        // add factor
                        cur_level[token[factor]] =
                            current_factor_level[factor_level_index]++;
                    }
                    ++factor_level_index;
                }
            }
        }
    }
    cov.close();
    // Here, we should know the identity of the valid sample and also
    // the factor levels
    // now calculate the number of column required
    // 1 for intercept, 1 for PRS
    // also output factor info

    std::vector<std::string> all_missing_cov;
    std::string message =
        "Include Covariates:\nName\tMissing\tNumber of levels\n";
    uint32_t total_column = 2;
    uint32_t num_sample = m_sample_with_phenotypes.size();
    factor_level_index = 0;
    uint32_t cur_cov_index = 0;
    uint32_t num_level = 0;
    for (auto&& cov : cov_index) {
        cov_start_index.push_back(total_column);
        if (factor_level_index == factor_cov_index.size()
            || cov != factor_cov_index[factor_level_index])
        {
            ++total_column;
            message.append(cov_name[cur_cov_index] + "\t"
                           + std::to_string(missing_count[cov]) + "\t-\n");
        }
        else
        {
            num_level = factor_levels[factor_level_index++].size();
            total_column += num_level - 1;
            message.append(cov_name[cur_cov_index] + "\t"
                           + std::to_string(missing_count[cov]) + "\t"
                           + std::to_string(num_level) + "\n");
        }
        ++cur_cov_index;
        // error messages here
    }
    reporter.report(message);
    // now update the m_phenotype vector
    if (valid_sample_index.size() != num_sample && num_sample != 0) {
        // helpful to give the overview
        int removed = num_sample - valid_sample_index.size();
        message =
            std::to_string(removed) + " sample(s) with invalid covariate:\n\n";
        double portion = (double) removed / (double) num_sample;
        if (valid_sample_index.size() == 0) {
            // if all samples are removed
            cur_cov_index = 0;
            for (auto&& cov : cov_index) {
                if (missing_count[cov] == num_sample) {
                    // we sorted the column index so we can't tell what the
                    // column name is useless we also store the head of the file
                    // (too troublesome)
                    message.append("Error: " + cov_name[cur_cov_index]
                                   + " is invalid, please check it is of the "
                                     "correct format\n");
                }
                ++cur_cov_index;
            }
            reporter.report(message);
            throw std::runtime_error("Error: All samples removed due to "
                                     "missingness in covariate file!");
        }
        if (portion > 0.05) {
            message.append(
                "Warning: More than " + std::to_string(portion * 100)
                + "% of your samples were removed! "
                  "You should check if your covariate file is correct\n");
        }
        reporter.report(message);
        // sort the sample index
        // Sorting is required because our ordering follows the covariate
        // file, which does not need to have the same ordering as the
        // target file.
        // Also, this does means that the base factor will be the
        // first factor observed within the covariate file, not
        // the one observed in the first sample
        std::sort(begin(valid_sample_index), end(valid_sample_index),
                  [](std::pair<std::string, size_t> const& t1,
                     std::pair<std::string, size_t> const& t2) {
                      if (std::get<1>(t1) == std::get<1>(t2))
                          return std::get<0>(t1).compare(std::get<0>(t2)) < 0;
                      else
                          return std::get<1>(t1) < std::get<1>(t2);
                  });


        // update the m_phenotype and m_independent
        m_sample_with_phenotypes.clear();
        // vector contains the name of samples that we keep
        // and also their original index on m_phenotype
        for (size_t cur_index = 0; cur_index < valid_sample_index.size();
             ++cur_index)
        {
            std::string name = std::get<0>(valid_sample_index[cur_index]);
            m_sample_with_phenotypes[name] = cur_index;
            size_t original_index = std::get<1>(valid_sample_index[cur_index]);
            if (original_index != cur_index) {
                m_phenotype(cur_index, 0) = m_phenotype(original_index, 0);
            }
        }
        m_phenotype.conservativeResize(valid_sample_index.size(), 1);
    }
    num_column = total_column;
}

void PRSice::gen_cov_matrix(const std::string& c_cov_file,
                            std::vector<std::string> cov_header_name,
                            std::vector<uint32_t> cov_header_index,
                            std::vector<uint32_t> factor_cov_index,
                            Reporter& reporter)
{
    // The size of the map should be informative of the number of sample
    size_t num_sample = m_sample_with_phenotypes.size();
    if (c_cov_file.empty()) {
        // if no covariates, just return a matrix of 1
        m_independent_variables = Eigen::MatrixXd::Ones(num_sample, 2);
        return;
    }
    // obtain the index of each covariate
    // the key is the variable name and the value is the index on the matrix
    // need to account for the situation where the same variable name can
    // occur in different covariates
    // As the index are sorted, we can use vector
    std::vector<std::unordered_map<std::string, uint32_t>> factor_list;
    std::vector<uint32_t> cov_start_index;
    uint32_t num_column = 2 + cov_header_index.size();
    if (factor_cov_index.size() != 0) {
        /*
         * What we want:
         * For each factor of a factor covariate, know which column should we
         * put the 1 to
         */
        process_cov_file(c_cov_file, factor_cov_index, cov_start_index,
                         cov_header_index, cov_header_name, factor_list,
                         num_column, reporter);
    }
    else
    {
        for (size_t i = 0; i < cov_header_index.size(); ++i) {
            cov_start_index.push_back(i + 2);
        }
    }
    std::string message = "Processing the covariate file: " + c_cov_file + "\n";
    message.append("==============================\n");
    reporter.report(message);
    m_independent_variables = Eigen::MatrixXd::Zero(num_sample, num_column);
    m_independent_variables.col(0).setOnes();
    m_independent_variables.col(1).setOnes();
    // now we only need to fill in the independent matrix without worry about
    // other stuff
    std::ifstream cov;
    cov.open(c_cov_file.c_str());
    if (!cov.is_open()) {
        std::string error_message =
            "Error: Cannot open covariate file: " + c_cov_file;
        throw std::runtime_error(error_message);
    }
    std::vector<std::string> token;
    std::string line, id;
    size_t max_index = cov_header_index.back() + 1, index, cur_index, f_level;
    uint32_t cur_cov_index = 0, cur_factor_index = 0,
             num_factor = factor_cov_index.size(),
             num_cov = cov_header_index.size();
    while (std::getline(cov, line)) {
        misc::trim(line);
        if (line.empty()) continue;
        token = misc::split(line);
        if (token.size() < max_index) {
            std::string error_message =
                "Error: Malformed covariate file, should contain at least "
                + std::to_string(max_index) + " column!";
            throw std::runtime_error(error_message);
        }
        id = (m_ignore_fid) ? token[0] : token[0] + "_" + token[1];
        if (m_sample_with_phenotypes.find(id) != m_sample_with_phenotypes.end())
        {
            // now we can propergate the matrix
            cur_cov_index = 0;
            cur_factor_index = 0;
            index = m_sample_with_phenotypes[id];
            for (size_t i_cov = 0; i_cov < num_cov; ++i_cov) {
                if (cur_factor_index >= num_factor
                    || cov_header_index[i_cov]
                           != factor_cov_index[cur_factor_index])
                {
                    // noraml covariate
                    m_independent_variables(index, cov_start_index[i_cov]) =
                        misc::convert<double>(token[cov_header_index[i_cov]]);
                }
                else
                {
                    f_level = factor_list[cur_factor_index]
                                         [token[cov_header_index[i_cov]]];
                    if (f_level != 0) {
                        cur_index = cov_start_index[i_cov] + f_level - 1;
                        m_independent_variables(index, cur_index) = 1;
                    }
                    ++cur_factor_index;
                }
            }
        }
    }

    message = "After reading the covariate file, "
              + std::to_string(m_sample_with_phenotypes.size())
              + " sample(s) included in the analysis\n";
    reporter.report(message);
}

void PRSice::run_prsice(const Commander& c_commander, const Region& region,
                        const size_t pheno_index, const size_t region_index,
                        Genotype& target)
{
    // target.reset_sample_prs();
    // prslice can easily be implemented using PRSet functionality
    // so maybe remove prslice from this function
    const bool no_regress = c_commander.no_regress();
    const bool print_all_scores = c_commander.all_scores();
    const int num_thread = c_commander.thread();
    const bool multi = pheno_info.name.size() > 1;
    const size_t num_samples_included = target.num_sample();
    const bool cumulate = c_commander.cumulate();
    Eigen::initParallel();
    Eigen::setNbThreads(num_thread);
    m_best_index = -1;
    m_num_snp_included = 0;
    m_perm_result.resize(m_num_perm, 2);
    m_best_sample_score.clear();
    // if m_prs_results is small (or 0), initialize by resize
    // otherwise, resize do nothing, but then we can change p-threshold to -1
    // because we reset best_index to -1, we can safely ignore reseting other
    // fields (e.g. r2) because we will overwrite it anyway
    m_prs_results.resize(target.num_threshold());
    // set to -1 to indicate not done
    for (auto&& p : m_prs_results) {
        p.threshold = -1;
        p.r2 = 0.0;
        p.num_snp = 0;
    }
    // initialize score vector
    m_best_sample_score.resize(target.num_sample());

    // now prepare all score
    // in theory, we only need to calulate it once for every phenotype + sets
    // but it is easier to do it this way
    std::fstream all_out;
    if (print_all_scores) {
        std::string all_out_name = c_commander.out();
        if (multi) {
            all_out_name.append("." + pheno_info.name[pheno_index]);
        }
        all_out_name.append(".all.score");
        all_out.open(all_out_name.c_str(),
                     std::fstream::out | std::fstream::in | std::fstream::ate);
        if (!all_out.is_open()) {
            std::string error_message =
                "Cannot open file " + all_out_name + " for write";
            throw std::runtime_error(error_message);
        }
    }


    // current threshold iteration
    size_t iter_threshold = 0;
    // +1 such that only 100% when finished

    int cur_category = 0, cur_index = -1;
    double cur_threshold = 0.0;
    bool require_standardize = (m_score == SCORING::STANDARDIZE);
    print_progress();
    bool first_run = true;
    while (target.get_score(cur_index, cur_category, cur_threshold,
                            m_num_snp_included, region_index, cumulate,
                            require_standardize, first_run))
    {
        m_analysis_done++;
        print_progress();

        if (print_all_scores) {
            for (size_t sample = 0; sample < num_samples_included; ++sample) {
                double score = target.calculate_score(m_score, sample);
                size_t loc = m_all_file.header_length
                             + sample * (m_all_file.line_width + NEXT_LENGTH)
                             + NEXT_LENGTH + m_all_file.skip_column_length
                             + m_all_file.processed_threshold
                             + m_all_file.processed_threshold * m_numeric_width;
                all_out.seekp(loc);
                all_out << std::setprecision(m_precision) << score;
            }
        }
        m_all_file.processed_threshold++;
        if (no_regress) {
            iter_threshold++;
            continue;
        }
        regress_score(target, cur_threshold, num_thread, pheno_index,
                      iter_threshold);

        if (c_commander.permutation() != 0) {
            permutation(target, num_thread, m_target_binary[pheno_index]);
        }
        iter_threshold++;
        first_run = false;
    }

    if (all_out.is_open()) all_out.close();
    if (c_commander.permutation() != 0) process_permutations();
    if (!no_regress) {
        print_best(target, pheno_index, c_commander);
        // we don't do competitive for the full set
        /*
        if (m_prset && c_commander.perform_set_perm() && region_index != 0) {
            run_competitive(target, c_commander,
                            region.num_post_clump_snp(region_index),
                            region.duplicated_size(region_index),
                            m_target_binary[pheno_index]);
        }
        */
    }
}

void PRSice::print_best(Genotype& target, const size_t pheno_index,
                        const Commander& commander)
{

    std::string pheno_name =
        (pheno_info.name.size() > 1) ? pheno_info.name[pheno_index] : "";
    std::string output_prefix = commander.out();
    if (!pheno_name.empty()) output_prefix.append("." + pheno_name);
    std::string out_best = output_prefix + ".best";
    std::fstream best_out(out_best.c_str(), std::fstream::out | std::fstream::in
                                                | std::fstream::ate);
    auto&& best_info = m_prs_results[m_best_index];
    int best_snp_size = best_info.num_snp;
    if (best_snp_size == 0) {
        fprintf(stderr, "Error: Best R2 obtained when no SNPs were included\n");
        fprintf(stderr, "       Cannot output the best PRS score\n");
    }
    else
    {
        for (size_t sample = 0; sample < target.num_sample(); ++sample) {
            // samples that are extracted are ignored
            // sample excluded will not be output here
            std::string has_pheno =
                target.sample_in_regression(sample) ? "Yes" : "No";
            size_t loc = m_best_file.header_length
                         + sample * (m_best_file.line_width + NEXT_LENGTH)
                         + NEXT_LENGTH + m_best_file.skip_column_length
                         + m_best_file.processed_threshold
                         + m_best_file.processed_threshold * m_numeric_width;

            best_out.seekp(loc);
            best_out << std::setprecision(m_precision)
                     << m_best_sample_score[sample];
        }
    }
    best_out.close();
    m_best_file.processed_threshold++;
}

void PRSice::regress_score(Genotype& target, const double threshold,
                           size_t thread, const size_t pheno_index,
                           const size_t iter_threshold)
{
    double r2 = 0.0, r2_adjust = 0.0, p_value = 0.0, coefficient = 0.0,
           se = 0.0;
    const size_t num_regress_samples = m_matrix_index.size();
    if (m_num_snp_included == 0
        || (m_num_snp_included == m_prs_results[iter_threshold].num_snp))
    {
        return; // didn't got extra SNPs to process
    }

    for (size_t sample_id = 0; sample_id < num_regress_samples; ++sample_id) {
        std::string sample = target.sample_id(sample_id);
        m_independent_variables(sample_id, 1) =
            target.calculate_score(m_score, m_matrix_index[sample_id]);
    }

    if (m_target_binary[pheno_index]) {
        try
        {
            Regression::glm(m_phenotype, m_independent_variables, p_value, r2,
                            coefficient, se, 25, thread, true);
        }
        catch (const std::runtime_error& error)
        {
            // This should only happen when the glm doesn't converge.
            // Let's hope that won't happen...
            fprintf(stderr, "Error: GLM model did not converge!\n");
            fprintf(stderr, "       Please send me the DEBUG files\n");
            std::ofstream debug;
            debug.open("DEBUG");
            debug << m_independent_variables << "\n";
            debug.close();
            debug.open("DEBUG.y");
            debug << m_phenotype << "\n";
            debug.close();
            fprintf(stderr, "Error: %s\n", error.what());
        }
    }
    else
    {
        Regression::linear_regression(m_phenotype, m_independent_variables,
                                      p_value, r2, r2_adjust, coefficient, se,
                                      thread, true);
    }

    // If this is the best r2, then we will add it
    int best_index = m_best_index;
    if (iter_threshold == 0 || best_index < 0
        || m_prs_results[best_index].r2 < r2)
    {
        m_best_index = iter_threshold;
        // can't remember why I can't just copy the whole vector
        size_t num_include_samples = target.num_sample();
        for (size_t s = 0; s < num_include_samples; ++s) {
            m_best_sample_score[s] = target.calculate_score(m_score, s);
        }
    }
    prsice_result cur_result;
    cur_result.threshold = threshold;
    cur_result.r2 = r2;
    cur_result.r2_adj = r2_adjust;
    cur_result.coefficient = coefficient;
    cur_result.p = p_value;
    cur_result.emp_p = -1.0;
    cur_result.num_snp = m_num_snp_included;
    cur_result.se = se;
    cur_result.competitive_p = -1.0;
    m_prs_results[iter_threshold] = cur_result;
}


void PRSice::process_permutations()
{
    // can't generate an empirical p-value if there is no observed p-value
    if (m_best_index == -1) return;
    // double best_p = m_prs_results[m_best_index].p;
    double best_t = m_prs_results[m_best_index].coefficient
                    / m_prs_results[m_best_index].se;
    size_t num_better = 0;
    num_better = std::count_if(m_perm_result.begin(), m_perm_result.end(),
                               [&best_t](double t) { return t > best_t; });
    // for (auto&& p : m_perm_result) num_better += (p <= best_p);
    m_prs_results[m_best_index].emp_p =
        (double) (num_better + 1.0) / (double) (m_num_perm + 1.0);
}

void PRSice::permutation(Genotype& target, const size_t n_thread,
                         bool is_binary)
{

    Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic> perm_matrix(
        m_phenotype.rows());
    Eigen::setNbThreads(n_thread);
    int rank = 0;
    // logit_perm can only be true if it is binary trait and user used the
    // --logit-perm flag
    // can always do the following if
    // 1. QT trait (!is_binary)
    // 2. Not require logit perm
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> decomposed;
    Eigen::VectorXd pre_se_calulated;
    bool run_glm = true;
    if (!is_binary || !m_logit_perm) {
        decomposed.compute(m_independent_variables);
        rank = decomposed.rank();
        Eigen::MatrixXd R = decomposed.matrixR()
                                .topLeftCorner(rank, rank)
                                .triangularView<Eigen::Upper>();
        pre_se_calulated = (R.transpose() * R).inverse().diagonal();
        run_glm = false;
    }
    if (n_thread == 1) {
        run_null_perm_no_thread(decomposed, rank, pre_se_calulated, run_glm);
    }
    else
    {
        Thread_Queue<std::pair<Eigen::VectorXd, size_t>> set_perm_queue;
        std::thread producer(&PRSice::gen_null_pheno, this,
                             std::ref(set_perm_queue), n_thread - 1);
        std::vector<std::thread> consume_store;
        for (size_t i = 0; i < n_thread - 1; ++i) {
            consume_store.push_back(
                std::thread(&PRSice::consume_null_pheno, this,
                            std::ref(set_perm_queue), std::ref(decomposed),
                            rank, std::cref(pre_se_calulated), run_glm));
        }
        producer.join();
        for (auto&& consume : consume_store) consume.join();
    }
}

void PRSice::run_null_perm_no_thread(
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd>& decomposed, int rank,
    const Eigen::VectorXd& pre_se, bool run_glm)
{
    size_t processed = 0;
    std::mt19937 rand_gen{m_seed};
    // Eigen::setNbThreads(1);
    const size_t num_regress_sample = m_phenotype.rows();
    const bool intercept = true;
    Eigen::VectorXd perm_pheno = m_phenotype;
    if (run_glm) {
        while (processed < m_num_perm) {
            perm_pheno = m_phenotype;
            std::shuffle(perm_pheno.data(),
                         perm_pheno.data() + num_regress_sample, rand_gen);
            m_analysis_done++;
            print_progress();
            double coefficient, se, r2, obs_p;
            // double obs_p = 2.0; // for safety reason, make sure it is out
            // bound
            double obs_t = -1;
            Regression::glm(perm_pheno, m_independent_variables, obs_p, r2,
                            coefficient, se, 25, 1, true);
            obs_t = coefficient / se;
            m_perm_result[processed] =
                std::min(obs_t, m_perm_result[processed]);
            processed++;
        }
    }
    else
    {
        Eigen::VectorXd beta;
        Eigen::VectorXd se;
        while (processed < m_num_perm) {
            // perm_pheno = m_phenotype;
            std::shuffle(perm_pheno.data(),
                         perm_pheno.data() + num_regress_sample, rand_gen);
            m_analysis_done++;
            print_progress();
            // double obs_p = 2.0; // for safety reason, make sure it is out
            // bound
            double obs_t = -1;
            beta = decomposed.solve(perm_pheno);
            int rdf = num_regress_sample - rank;
            double rss =
                (m_independent_variables * beta - perm_pheno).squaredNorm();
            int se_index = intercept;
            for (int ind = 0; ind < beta.rows(); ++ind) {
                if (decomposed.colsPermutation().indices()(ind) == intercept) {
                    se_index = ind;
                    break;
                }
            }
            double resvar = rss / (double) rdf;
            se = (pre_se * resvar).array().sqrt();
            obs_t = std::abs(beta(intercept) / se(se_index));
            m_perm_result[processed] =
                std::min(obs_t, m_perm_result[processed]);
            processed++;
        }
    }
}

void PRSice::gen_null_pheno(Thread_Queue<std::pair<Eigen::VectorXd, size_t>>& q,
                            size_t num_consumer)
{
    size_t processed = 0;
    std::mt19937 rand_gen{m_seed};
    Eigen::setNbThreads(1);
    const size_t num_regress_sample = m_phenotype.rows();
    Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic> perm_matrix(
        m_phenotype.rows());

    while (processed < m_num_perm) {
        // make sure it doesn't cause some crazy error
        Eigen::VectorXd null_pheno = m_phenotype;
        std::shuffle(null_pheno.data(), null_pheno.data() + num_regress_sample,
                     rand_gen);
        // q.push(p, num_consumer);
        q.emplace(std::make_pair(null_pheno, processed), num_consumer);
        m_analysis_done++;
        print_progress();
        processed++;
    }
    // send termination signal to the consumers
    for (size_t i = 0; i < num_consumer; ++i) {
        q.push(std::pair<Eigen::VectorXd, size_t>(Eigen::VectorXd(1), 0),
               num_consumer);
    }
}

void PRSice::consume_null_pheno(
    Thread_Queue<std::pair<Eigen::VectorXd, size_t>>& q,
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd>& decomposed, int rank,
    const Eigen::VectorXd& pre_se, bool run_glm)
{
    const size_t n = m_phenotype.rows();
    const bool intercept = true;
    std::vector<double> temp_store;
    std::vector<size_t> temp_index;
    Eigen::VectorXd beta, se;
    int rdf = n - rank;
    while (true) {
        std::pair<Eigen::VectorXd, size_t> input;
        q.pop(input);
        // all job finished

        if (std::get<0>(input).rows() == 1) break;
        double coefficient, se_res, r2, obs_p;
        // double obs_p = 2.0; // for safety reason, make sure it is out bound
        double obs_t = -1;
        if (run_glm) {
            Regression::glm(std::get<0>(input), m_independent_variables, obs_p,
                            r2, coefficient, se_res, 25, 1, true);
            obs_t = coefficient / se_res;
        }
        else
        {

            beta = decomposed.solve(std::get<0>(input));
            // Eigen::MatrixXd fitted = m_independent_variables * beta;
            double rss = (m_independent_variables * beta - std::get<0>(input))
                             .squaredNorm();
            size_t se_index = intercept;
            for (size_t ind = 0; ind < (size_t) beta.rows(); ++ind) {
                if (decomposed.colsPermutation().indices()(ind) == intercept) {
                    se_index = ind;
                    break;
                }
            }
            double resvar = rss / (double) rdf;
            se = (pre_se * resvar).array().sqrt();
            obs_t = std::fabs(beta(intercept) / se(se_index));
            // obs_p = misc::calc_tprob(tval, n);
        }
        temp_store.push_back(obs_t);
        temp_index.push_back(std::get<1>(input));
    }

    std::lock_guard<std::mutex> lock(lock_guard);
    for (size_t i = 0; i < temp_store.size(); ++i) {
        double obs_t = temp_store[i];
        auto&& index = temp_index[i];
        if (m_perm_result[index] < obs_t) {
            m_perm_result[index] = obs_t;
        }
    }
}
void PRSice::thread_perm(
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd>& decomposed, size_t start,
    size_t end, int rank, const Eigen::VectorXd& pre_se, size_t processed)
{
    bool intercept = true;
    size_t n = m_independent_variables.rows();
    std::vector<double> temp_store;
    temp_store.reserve(end - start);

    for (size_t i = start; i < end; ++i) {
        double* perm_pheno_ptr = m_permuted_pheno.data();
        perm_pheno_ptr = &(perm_pheno_ptr[i * n]);
        Eigen::Map<Eigen::VectorXd> perm_pheno(perm_pheno_ptr, n);
        double obs_p = 2.0; // for safety reason, make sure it is out bound
        if (m_logit_perm) {
            double r2, coefficient, se;
            Regression::glm(perm_pheno, m_independent_variables, obs_p, r2,
                            coefficient, se, 25, 1, true);
        }
        else
        {
            Eigen::VectorXd beta = decomposed.solve(perm_pheno);
            Eigen::MatrixXd fitted = m_independent_variables * beta;

            Eigen::VectorXd residual = perm_pheno - fitted;
            int rdf = n - rank;
            double rss = 0.0;
            for (size_t r = 0; r < n; ++r) {
                rss += residual(r) * residual(r);
            }
            size_t se_index = intercept;
            for (size_t ind = 0; ind < (size_t) beta.rows(); ++ind) {
                if (decomposed.colsPermutation().indices()(ind) == intercept) {
                    se_index = ind;
                    break;
                }
            }
            double resvar = rss / (double) rdf;
            Eigen::VectorXd se = (pre_se * resvar).array().sqrt();
            double tval = beta(intercept) / se(se_index);
            obs_p = misc::calc_tprob(tval, n);
        }
        // store the best p_value for the processed+i permutaiton
        // this is thread safe as we will never actually touch any overlapped
        // area
        temp_store.push_back(obs_p);
    }
    int index = 0;
    // this might seems odd, but we put it here to minimize false sharing (best
    // if mutex)
    std::lock_guard<std::mutex> lock(lock_guard);
    for (size_t i = start; i < end; ++i) {
        double obs_p = temp_store[index++];
        double ori_p = m_perm_result[processed + i];
        m_perm_result[processed + i] = (ori_p > obs_p) ? obs_p : ori_p;
    }
}


void PRSice::prep_output(const Commander& c_commander, Genotype& target,
                         std::vector<std::string> region_name,
                         const size_t pheno_index)
{
    // As R has a default precision of 7, we will go a bit
    // higher to ensure we use up all precision
    std::string pheno_name =
        (pheno_info.name.size() > 1) ? pheno_info.name[pheno_index] : "";
    std::string output_prefix = c_commander.out();
    if (!pheno_name.empty()) output_prefix.append("." + pheno_name);
    const bool perm = (c_commander.permutation() != 0);
    std::string output_name = output_prefix;
    std::string out_prsice = output_name + ".prsice";
    std::string out_all = output_name + ".all.score";
    std::string out_best = output_name + ".best";
    std::ofstream prsice_out, best_out, all_out;

    // .prsice output
    prsice_out.open(out_prsice.c_str());
    if (!prsice_out.is_open()) {
        std::string error_message =
            "Error: Cannot open file: " + out_prsice + " to write";
        throw std::runtime_error(error_message);
    }
    prsice_out << "Set\tThreshold\tR2\tP\tCoefficient\tStandard.Error\tNum_SNP";
    // if (m_prset) prsice_out << "\tCompetitive.P";
    if (perm) prsice_out << "\tEmpirical_P";
    prsice_out << "\n";
    prsice_out.close();

    // .best output
    best_out.open(out_best.c_str());
    if (!best_out.is_open()) {
        std::string error_message =
            "Error: Cannot open file: " + out_best + " to write";
        throw std::runtime_error(error_message);
    }
    std::string header_line = "FID IID In_Regression";
    // if not preset, then it is PRS,otherwise, it will be the
    if (!m_prset)
        header_line.append(" PRS");
    else
    {
        for (size_t i = 0; i < region_name.size() - 1; ++i) {
            header_line.append(" " + region_name[i]);
        }
    }
    best_out << header_line << "\n";
    m_best_file.header_length = header_line.length() + 1;
    m_best_file.processed_threshold = 0;
    // each numeric output took 12 spaces, then for each output, there is one
    // space next to each

    m_best_file.line_width = m_max_fid_length + 1 + m_max_iid_length + 1 + 3 + 1
                             + region_name.size() * (m_numeric_width + 1) + 1;

    m_best_file.skip_column_length =
        m_max_fid_length + 1 + m_max_iid_length + 1 + 3 + 1;


    // also handle all score here
    const bool all_scores = c_commander.all_scores();
    if (all_scores) {
        all_out.open(out_all.c_str());
        if (!all_out.is_open()) {
            std::string error_message =
                "Cannot open file " + out_all + " for write";
            throw std::runtime_error(error_message);
        }
        std::vector<double> avail_thresholds = target.get_thresholds();
        std::sort(avail_thresholds.begin(), avail_thresholds.end());
        size_t num_thresholds = avail_thresholds.size();
        header_line = "FID IID";
        if (!m_prset) {
            for (auto& thres : avail_thresholds) {
                header_line.append(" " + std::to_string(thres));
            }
        }
        else
        {
            for (size_t i = 0; i < region_name.size() - 1; ++i) {
                for (auto& thres : avail_thresholds) {
                    header_line.append(" " + region_name[i] + "_"
                                       + std::to_string(thres));
                }
            }
        }
        m_all_file.header_length = header_line.length() + 1;
        m_all_file.processed_threshold = 0;
        m_all_file.line_width =
            m_max_fid_length + 1 + m_max_iid_length + 1
            + num_thresholds * region_name.size() * (m_numeric_width + 1) + 1;
        m_all_file.skip_column_length = m_max_fid_length + m_max_iid_length + 2;
        all_out << header_line << "\n";
    }

    // output sample IDs
    size_t num_samples_included = target.num_sample();
    for (size_t i_sample = 0; i_sample < num_samples_included; ++i_sample) {
        std::string name = target.fid(i_sample) + " " + target.iid(i_sample);
        std::string best_line =
            name + " "
            + ((target.sample_in_regression(i_sample)) ? "Yes" : "No");
        best_out << std::setfill(' ') << std::setw(m_best_file.line_width)
                 << std::left << best_line << "\n";
        if (all_scores) {
            all_out << std::setfill(' ') << std::setw(m_all_file.line_width)
                    << std::left << name << "\n";
        }
    }
    m_all_file.line_width++;
    m_best_file.line_width++; // now account for new line
    best_out.close();
    if (all_out.is_open()) all_out.close();
}
void PRSice::output(const Commander& c_commander, const Region& region,
                    const size_t pheno_index, const size_t region_index,
                    Genotype& target)
{
    std::vector<double> prev = c_commander.prevalence();
    bool has_prevalence = (prev.size() != 0);
    has_prevalence = has_prevalence && c_commander.is_binary(pheno_index);
    double top = 1.0, bottom = 1.0, prevalence = -1;
    if (has_prevalence) {
        size_t num_binary = 0;
        for (size_t i = 0; i < pheno_index; ++i) {
            if (c_commander.is_binary(i))
                num_binary++; // this is the number of previous binary traits
        }
        int num_case = 0, num_control = 0;
        for (size_t i = 0; i < (size_t) m_phenotype.rows(); ++i) {
            if (m_phenotype(i) == 0)
                num_control++;
            else if (m_phenotype(i) == 1)
                num_case++;
        }
        double case_ratio =
            (double) (num_case) / (double) (num_case + num_control);
        prevalence = prev[num_binary];
        double x = misc::qnorm(1 - prevalence);
        double z = misc::dnorm(x);
        double i2 = z / prevalence;
        double cc = prevalence * (1 - prevalence) * prevalence
                    * (1 - prevalence)
                    / (z * z * case_ratio * (1 - case_ratio));
        double theta =
            i2 * ((case_ratio - prevalence) / (1 - prevalence))
            * (i2 * ((case_ratio - prevalence) / (1 - prevalence)) - x);
        double e = 1
                   - pow(case_ratio, (2 * case_ratio))
                         * pow((1 - case_ratio), (2 * (1 - case_ratio)));
        top = cc * e;
        bottom = cc * e * theta;
    }

    std::string pheno_name =
        (pheno_info.name.size() > 1) ? pheno_info.name[pheno_index] : "";
    std::string output_prefix = c_commander.out();
    if (!pheno_name.empty()) output_prefix.append("." + pheno_name);

    const bool perm = (c_commander.permutation() != 0);

    bool valid = m_best_index != -1;
    if (!valid
        || region.get_count(region_index)
               == 0) // we know regions with 0 SNP will not have valid PRS
    {
        if (region.get_count(region_index) != 0) {
            fprintf(stderr, "Error: No valid PRS ");
            if (m_prset)
                fprintf(stderr, "for %s",
                        region.get_name(region_index).c_str());
            fprintf(stderr, "!\n");
        }
        return;
    }
    std::string out_prsice = output_prefix + ".prsice";
    std::string out_snp = output_prefix + ".snps";
    // std::string out_summary = output_name + ".summary";
    std::ofstream prsice_out, snp_out;
    prsice_out.open(out_prsice.c_str(), std::fstream::app);
    if (!prsice_out.is_open()) {
        std::string error_message =
            "Error: Cannot open file: " + out_prsice + " to write";
        throw std::runtime_error(error_message);
    }

    for (size_t i = 0; i < m_prs_results.size(); ++i) {
        if (m_prs_results[i].threshold < 0 || m_prs_results[i].p < 0) continue;
        double full = m_prs_results[i].r2;
        double null = m_null_r2;
        if (has_prevalence) {
            full = top * full / (1 + bottom * full);
            null = top * null / (1 + bottom * null);
        }
        double r2 = full - null;
        prsice_out << region.get_name(region_index) << "\t"
                   << m_prs_results[i].threshold << "\t" << r2 << "\t"
                   << m_prs_results[i].p << "\t" << m_prs_results[i].coefficient
                   << "\t" << m_prs_results[i].se << "\t"
                   << m_prs_results[i].num_snp;
        /*
        if (m_prset && (m_prs_results[i].competitive_p >= 0))
            prsice_out << "\t" << m_prs_results[i].competitive_p;
        else if (m_prset)
            prsice_out << "\t-";
            */
        if (perm && (m_prs_results[i].emp_p >= 0.0))
            prsice_out << "\t" << m_prs_results[i].emp_p;
        else if (perm)
            prsice_out << "\t-";
        prsice_out << "\n";
    }
    prsice_out.close();
    auto&& best_info = m_prs_results[m_best_index];


    prsice_summary prs_sum;
    prs_sum.pheno = pheno_name;
    prs_sum.set = region.get_name(region_index);
    prs_sum.result = best_info;
    // prs_sum.result.r2 = best_info.r2;

    // prs_sum.result.competitive_p = best_info.competitive_p;
    prs_sum.r2_null = m_null_r2;
    prs_sum.top = top;
    prs_sum.bottom = bottom;
    prs_sum.prevalence = prevalence;
    m_prs_summary.push_back(prs_sum);

    if (best_info.p > 0.1)
        m_significant_store[0]++;
    else if (best_info.p > 1e-5)
        m_significant_store[1]++;
    else
        m_significant_store[2]++;
    /*
        if (c_commander.print_snp()) {
            target.print_snp(out_snp, m_prs_results[m_best_index].threshold,
                             region_index);
        }
        */
}

void PRSice::summarize(const Commander& commander, Reporter& reporter)
{
    bool prev_out = false;

    const bool perm = (commander.permutation() != 0);
    std::string message = "There are ";
    if (m_significant_store[0] != 0) {
        message.append(std::to_string(m_significant_store[0])
                       + " region(s) with p-value > 0.1 (\033[1;31mnot "
                         "significant\033[0m);");
        prev_out = true;
    }
    if (m_significant_store[1] != 0) {
        if (m_significant_store[2] == 0 && prev_out) {
            message.append(" and ");
        }
        message.append(
            std::to_string(m_significant_store[1])
            + " region(s) with p-value between "
              "0.1 and 1e-5 (\033[1;31mmay not be significant\033[0m);");
        prev_out = true;
    }
    if (m_significant_store[2] != 0) {
        if (prev_out) message.append(" and ");
        message.append(std::to_string(m_significant_store[2])
                       + " region(s) with p-value less than 1e-5.");
    }
    if (!perm) {
        message.append(
            " Please note that these results are inflated due to the "
            "overfitting inherent in finding the best-fit "
            "PRS (but it's still best to find the best-fit PRS!).\n"
            "You can use the --perm option (see manual) to calculate "
            "an empirical P-value.");
    }
    reporter.report(message);
    std::string out_name = commander.out() + ".summary";
    std::ofstream out;
    out.open(out_name.c_str());
    if (!out.is_open()) {
        std::string error_message =
            "Error: Cannot open file: " + out_name + " to write";
        throw std::runtime_error(error_message);
    }
    out << "Phenotype\tSet\tThreshold\tPRS.R2\tFull.R2\tNull."
           "R2\tPrevalence\tCoefficient\tStandard.Error\tP\tNum_SNP";
    if (m_prset) out << "\tCompetitive.P";
    if (perm) out << "\tEmpirical-P";
    out << "\n";
    for (auto&& sum : m_prs_summary) {
        out << ((sum.pheno.empty()) ? "-" : sum.pheno) << "\t" << sum.set
            << "\t" << sum.result.threshold;
        if (sum.prevalence > 0) {
            double full = sum.result.r2;
            double null = sum.r2_null;
            full = sum.top * full / (1 + sum.bottom * full);
            null = sum.top * null / (1 + sum.bottom * null);
            out << "\t" << full - null << "\t" << full << "\t" << null << "\t"
                << sum.prevalence;
        }
        else
        {
            out << "\t" << sum.result.r2 - sum.r2_null << "\t" << sum.result.r2
                << "\t" << sum.r2_null << "\t-";
        }
        out << "\t" << sum.result.coefficient << "\t" << sum.result.se << "\t"
            << sum.result.p << "\t" << sum.result.num_snp;
        if (m_prset && (sum.result.competitive_p >= 0.0)) {
            out << "\t" << sum.result.competitive_p;
        }
        else if (m_prset)
        {
            out << "\t-";
        }
        if (perm) out << "\t" << sum.result.emp_p;
        out << "\n";
    }
    out.close();
}

PRSice::~PRSice()
{
    // dtor
}

void PRSice::gen_perm_memory(const Commander& commander, const size_t sample_ct,
                             Reporter& reporter)
{
    intptr_t min_memory_byte = 8 * sample_ct;
    intptr_t max_req_memory = min_memory_byte * m_num_perm;
    size_t malloc_size = misc::total_ram_available();
    // * 0.5 to provide room of error
    size_t valid_memory = commander.max_memory(malloc_size);
    intptr_t final_mb = (valid_memory - misc::current_ram_usage()) * 0.5;
    // start update here
    if (final_mb < 0) {
        throw std::runtime_error("Error: Insufficient memory for permutation!");
    }
    if (final_mb < min_memory_byte) {
        m_perm_per_slice = 1;
    }
    else if (final_mb > max_req_memory)
    {
        m_perm_per_slice = m_num_perm;
    }
    else
    {
        m_perm_per_slice = final_mb / min_memory_byte;
    }
    if (m_perm_per_slice * sample_ct > m_permuted_pheno.max_size()) {
        m_perm_per_slice = m_permuted_pheno.max_size() / sample_ct;
    }
    std::string message =
        std::to_string(((final_mb > max_req_memory) ? max_req_memory : final_mb)
                       / 1048576.0)
        + " MB RAM reserved for permutation\n";
    reporter.report(message);
    // wanna use double vector here as the sample size here might not be
    // the one used in the permutation. This might then lead to problem
    // in the permutation (segmentation fault, etc)
    m_permuted_pheno.resize(sample_ct * m_perm_per_slice);
    // g_num_snps.resize(g_max_threshold_store * m_sample_names.size(), 0);
    // g_prs_storage.resize(g_max_threshold_store * m_sample_names.size(), 0.0);
}
void PRSice::null_set_no_thread(
    Genotype& target, std::map<uint32_t, std::vector<uint32_t>>& set_index,
    std::vector<double>& ori_t_value, std::vector<uint32_t>& set_perm_res,
    const size_t num_perm, const bool is_binary, const bool require_standardize)
{
    const uint32_t max_size = set_index.rbegin()->first;
    size_t processed = 0;
    const size_t num_sample = m_matrix_index.size();
    double coefficient, se, r2, r2_adjust, obs_p, t_value;
    std::mt19937 g(m_seed);
    const size_t num_background = target.num_background();
    std::vector<size_t> background = target.background_index();
    bool first_run = true;
    while (processed < num_perm) {
        size_t begin = 0;
        // we will shuffle n where n is the set with the largest size
        size_t num_snp = max_size;
        while (num_snp--) {
            std::uniform_int_distribution<int> dist(begin, num_background - 1);
            size_t advance_index = dist(g);
            std::swap(background[begin], background[advance_index]);
            ++begin;
        }
        // now we have sorted the whole vector
        first_run = true;
        size_t prev_size = 0;
        for (auto&& set_size : set_index) {
            // now we iterate through each set size
            // in theory this will reduce our I/O. If the set sizes
            // are 10, 100 and 1000, then the number of SNPs we read
            // per set will be 10, 90, 900. Which will help to reduce
            // a lot of reading if the set sizes are very similar

            // read in genotype here
            target.get_null_score(set_size.first, prev_size, background,
                                  first_run, require_standardize);
            prev_size = set_size.first;
            for (size_t sample_id = 0; sample_id < num_sample; ++sample_id) {
                m_independent_variables(sample_id, 1) =
                    target.calculate_score(m_score, m_matrix_index[sample_id]);
            }
            m_analysis_done++;
            print_progress();
            if (is_binary) {
                Regression::glm(m_phenotype, m_independent_variables, obs_p, r2,
                                coefficient, se, 25, 1, true);
                t_value = std::abs(coefficient / se);
            }
            else
            {
                Regression::linear_regression(
                    m_phenotype, m_independent_variables, obs_p, r2, r2_adjust,
                    coefficient, se, 1, true);
                t_value = std::abs(coefficient / se);
            }
            // set_size second contain the indexs to each set with this size
            for (auto&& set_index : set_size.second) {
                set_perm_res[set_index] += (ori_t_value[set_index] < t_value);
            }
            first_run = false;
        }
        processed++;
    }
}
void PRSice::null_set_no_thread(Genotype& target, int& num_significant,
                                size_t num_perm, size_t set_size,
                                size_t num_selected_snps, double original_p,
                                bool require_standardize, bool is_binary,
                                bool store_p)
{

    size_t processed = 0;
    const size_t num_sample = m_matrix_index.size();
    double coefficient, se, r2, r2_adjust;
    std::mt19937 g(m_seed);
    const size_t num_background = target.num_background();
    std::vector<size_t> background = target.background_index();
    while (processed < num_perm) {
        size_t begin = 0;
        size_t num_snp = num_selected_snps;
        while (num_snp--) {
            std::uniform_int_distribution<int> dist(begin, num_background - 1);
            // size_t r = background[begin];
            size_t advance_index = dist(g);
            std::swap(background[begin], background[advance_index]);
            // background[begin] = background[advance_index];
            // background[advance_index] = r;
            ++begin;
        }
        // num_selected_snps = for if we use multiple threshold
        /*
        target.get_null_score(set_size, num_selected_snps, background,
                              require_standardize);
        */
        for (size_t sample_id = 0; sample_id < num_sample; ++sample_id) {
            m_independent_variables(sample_id, 1) =
                target.calculate_score(m_score, m_matrix_index[sample_id]);
        }
        m_analysis_done++;

        print_progress();
        double obs_p = 2.0; // for safety reason, make sure it is out bound
        if (is_binary) {
            Regression::glm(m_phenotype, m_independent_variables, obs_p, r2,
                            coefficient, se, 25, 1, true);
        }
        else
        {
            Regression::linear_regression(m_phenotype, m_independent_variables,
                                          obs_p, r2, r2_adjust, coefficient, se,
                                          1, true);
        }
        // thread_mutex
        num_significant += (original_p > obs_p);
        // if (store_p) null_p_value.push_back(obs_p);
        processed++;
    }
}

void PRSice::produce_null_prs(
    Thread_Queue<std::pair<std::vector<double>, uint32_t>>& q, Genotype& target,
    size_t num_consumer, std::map<uint32_t, std::vector<uint32_t>>& set_index,
    const size_t num_perm, const bool require_standardize)
{
    const uint32_t max_size = set_index.rbegin()->first;
    const size_t num_sample = m_matrix_index.size();
    const size_t num_regress_sample = m_independent_variables.rows();
    const size_t num_background = target.num_background();
    size_t processed = 0;
    size_t prev_size = 0;
    std::mt19937 g(m_seed);
    std::vector<size_t> background = target.background_index();
    bool first_run = true;
    while (processed < num_perm) {
        size_t begin = 0;
        // size_t num_snp = set_size;
        size_t num_snp = max_size;
        while (num_snp--) {
            std::uniform_int_distribution<int> dist(begin, num_background - 1);
            size_t r = background[begin];
            size_t advance_index = dist(g);
            background[begin] = background[advance_index];
            background[advance_index] = r;
            ++begin;
        }
        first_run = true;
        prev_size = 0;
        for (auto&& set_size : set_index) {
            target.get_null_score(set_size.first, prev_size, background,
                                  first_run, require_standardize);
            prev_size = set_size.first;
            std::vector<double> prs(num_regress_sample, 0);
            for (size_t sample_id = 0; sample_id < num_sample; ++sample_id) {
                prs[sample_id] =
                    target.calculate_score(m_score, m_matrix_index[sample_id]);
            }
            q.emplace(std::make_pair(prs, set_size.first), num_consumer);
            m_analysis_done++;
            print_progress();
            first_run = false;
        }
        processed++;
    }
    // send termination signal to the consumers
    for (size_t i = 0; i < num_consumer; ++i) {
        q.emplace(std::make_pair(std::vector<double>(), 0), num_consumer);
    }
}
// might want to remove num_selected_snps?
/*
void PRSice::produce_null_prs(Thread_Queue<std::vector<double>>& q,
                              Genotype& target, size_t num_consumer,
                              size_t num_perm, size_t set_size,
                              size_t num_selected_snps, double original_p,
                              bool require_standardize)
{
    size_t processed = 0;
    const size_t num_sample = m_matrix_index.size();
    const size_t num_regress_sample = m_independent_variables.rows();

    std::mt19937 g(m_seed);
    const size_t num_background = target.num_background();
    std::vector<size_t> background = target.background_index();
    while (processed < num_perm) {
        size_t begin = 0;
        // size_t num_snp = set_size;
        size_t num_snp = num_selected_snps;
        while (num_snp--) {
            std::uniform_int_distribution<int> dist(begin, num_background - 1);
            size_t r = background[begin];
            size_t advance_index = dist(g);
            background[begin] = background[advance_index];
            background[advance_index] = r;
            ++begin;
        }
        target.get_null_score(set_size, num_selected_snps, background,
                              require_standardize);
        std::vector<double> prs(num_regress_sample, 0);
        for (size_t sample_id = 0; sample_id < num_sample; ++sample_id) {
            prs[sample_id] =
                target.calculate_score(m_score, m_matrix_index[sample_id]);
        }

        q.emplace(std::move(prs), num_consumer);
        m_analysis_done++;
        print_progress();
        processed++;
    }
    // send termination signal to the consumers
    for (size_t i = 0; i < num_consumer; ++i) {
        q.emplace(std::vector<double>(), num_consumer);
    }
}
*/
void PRSice::consume_prs(
    Thread_Queue<std::pair<std::vector<double>, uint32_t>>& q,
    std::map<uint32_t, std::vector<uint32_t>>& set_index,
    std::vector<double>& ori_t_value, std::vector<uint32_t>& set_perm_res,
    const bool is_binary)
{

    Eigen::MatrixXd independent = m_independent_variables;
    const size_t num_regress_sample = m_matrix_index.size();
    std::vector<uint32_t> temp_perm_res(set_perm_res.size(), 0);
    double coefficient, se, r2, r2_adjust;
    double obs_p = 2.0; // for safety reason, make sure it is out bound
    std::pair<std::vector<double>, uint32_t> prs_info;

    while (true) {
        q.pop(prs_info);
        if (std::get<0>(prs_info).empty()) {
            // all job finished
            break;
        }
        for (size_t i_sample = 0; i_sample < num_regress_sample; ++i_sample) {
            independent(i_sample, 1) = std::get<0>(prs_info)[i_sample];
        }
        if (is_binary) {
            Regression::glm(m_phenotype, independent, obs_p, r2, coefficient,
                            se, 25, 1, true);
        }
        else
        {
            Regression::linear_regression(m_phenotype, independent, obs_p, r2,
                                          r2_adjust, coefficient, se, 1, true);
        }
        double t_value = std::abs(coefficient / se);
        auto&& index = set_index[std::get<1>(prs_info)];
        for (auto&& ref : index) {
            temp_perm_res[ref] += (ori_t_value[ref] < t_value);
        }
    }

    {
        // keep mutex lock within this scope
        std::unique_lock<std::mutex> locker(m_thread_mutex);
        size_t num_sets = temp_perm_res.size();
        for (size_t i = 0; i < num_sets; ++i) {
            set_perm_res[i] += temp_perm_res[i];
        }
    }
}
/*
void PRSice::consume_prs(Thread_Queue<std::vector<double>>& q,
                         double original_p, int& num_significant,
                         bool is_binary, bool store_p)
{
    int cur_num_significant = 0;
    std::vector<double> cur_null_p;
    Eigen::MatrixXd independent = m_independent_variables;
    const size_t num_regress_sample = m_independent_variables.rows();
    while (true) {
        std::vector<double> prs;
        q.pop(prs);

        if (prs.empty()) {

            // all job finished
            break;
        }
        for (size_t i_sample = 0; i_sample < num_regress_sample; ++i_sample) {
            independent(i_sample, 1) = prs[i_sample];
        }
        double coefficient, se, r2, r2_adjust;
        double obs_p = 2.0; // for safety reason, make sure it is out bound
        if (is_binary) {
            Regression::glm(m_phenotype, independent, obs_p, r2, coefficient,
                            se, 25, 1, true);
        }
        else
        {
            Regression::linear_regression(m_phenotype, independent, obs_p, r2,
                                          r2_adjust, coefficient, se, 1, true);
        }
        // thread_mutex
        cur_num_significant += (original_p > obs_p);
        cur_null_p.push_back(obs_p);
    }

    {
        std::unique_lock<std::mutex> locker(m_thread_mutex);

        num_significant += cur_num_significant;
    }
}
*/

void PRSice::run_competitive(Genotype& target, const Commander& commander,
                             const size_t pheno_index)
{
    // here we know the R2 and p-value of all pathways
    // storage should be

    fprintf(stderr, "\nStart competitive permutation\n");
    const size_t num_perm = commander.set_perm();
    const bool require_standardize = (m_score == SCORING::STANDARDIZE);
    const bool is_binary = m_target_binary[pheno_index];
    std::vector<double> ori_t_value;
    std::vector<uint32_t> set_perm_res;
    ori_t_value.reserve(m_prs_summary.size());
    set_perm_res.reserve(m_prs_summary.size());
    std::map<uint32_t, std::vector<uint32_t>> set_index;
    size_t num_prs_res = m_prs_summary.size();
    // start at 1 to avoid the base set
    for (size_t i = 1; i < num_prs_res; ++i) {
        auto&& res = m_prs_summary[i].result;
        set_index[res.num_snp].push_back(ori_t_value.size());
        ori_t_value.push_back(std::abs(res.coefficient / res.se));
        set_perm_res.push_back(0);
    }
    // now we can run the competitive testing


    size_t num_thread = commander.thread();
    // resize the result
    // m_perm_result.resize(num_perm, 2);
    const size_t num_regress_sample = m_independent_variables.rows();
    // a super over estimation of the amount of memory we need per thread
    const size_t basic_memory_required_per_thread =
        num_regress_sample * sizeof(double)
        * (m_independent_variables.cols() * 6 + 15);

    const size_t total_memory = misc::total_ram_available();
    const size_t valid_memory = commander.max_memory(total_memory);
    const size_t used_memory = misc::current_ram_usage();
    if (valid_memory <= used_memory) {
        fprintf(stderr, "\n");
        throw std::runtime_error("Error: Not enough memory for permutation");
    }
    // artificially reduce available memory to avoid memory overflow
    // ideally, if whole PRSice is within the same memory pool, we will
    // not need to do this reduction
    const size_t available_memory = (valid_memory - used_memory) * 0.5;

    if (available_memory < basic_memory_required_per_thread) {
        fprintf(stderr, "\n");
        throw std::runtime_error("Error: Not enough memory for permutation");
    }
    // reduce number of threads to account for memory available
    if (available_memory / basic_memory_required_per_thread < num_thread) {
        num_thread = available_memory / basic_memory_required_per_thread;
    }
    // they will be pushing around the PRS and the number of SNPs for this PRS
    if (num_thread > 1) {
        Thread_Queue<std::pair<std::vector<double>, uint32_t>> set_perm_queue;
        std::thread producer(&PRSice::produce_null_prs, this,
                             std::ref(set_perm_queue), std::ref(target),
                             num_thread - 1, std::ref(set_index), num_perm,
                             require_standardize);

        std::vector<std::thread> consumer_store;

        for (size_t i_thread = 0; i_thread < num_thread - 1; ++i_thread) {
            consumer_store.push_back(std::thread(
                &PRSice::consume_prs, this, std::ref(set_perm_queue),
                std::ref(set_index), std::ref(ori_t_value),
                std::ref(set_perm_res), is_binary));
        }

        producer.join();
        for (auto&& thread : consumer_store) thread.join();
    }
    else
    {
        null_set_no_thread(target, set_index, ori_t_value, set_perm_res,
                           num_perm, is_binary, require_standardize);
    }
    for (size_t i = 1; i < num_prs_res; ++i) {
        auto&& res = m_prs_summary[i].result;
        res.competitive_p = (set_perm_res[i - 1] + 1.0) / (num_perm + 1.0);
    }
}

/*
void PRSice::run_competitive(Genotype& target, const Commander& commander,

                             const size_t set_size, const bool store_null,
                             const bool is_binary)
{
    const size_t num_perm = commander.set_perm();
    const bool require_standardize = (m_score == SCORING::STANDARDIZE);

    const double obs_p_value = m_prs_results[m_best_index].p;
    const size_t num_selected_snps = m_prs_results[m_best_index].num_snp;
    // We want to know if we have sufficient memory for the number of thread
    // specified
    size_t num_thread = commander.thread();
    // resize the result
    // m_perm_result.resize(num_perm, 2);
    const size_t num_regress_sample = m_independent_variables.rows();
    // a super over estimation of the amount of memory we need per thread
    const size_t basic_memory_required_per_thread =
        num_regress_sample * sizeof(double)
        * (m_independent_variables.cols() * 6 + 15);

    const size_t total_memory = misc::total_ram_available();
    const size_t valid_memory = commander.max_memory(total_memory);
    const size_t used_memory = misc::current_ram_usage();
    if (valid_memory <= used_memory) {
        fprintf(stderr, "\n");
        throw std::runtime_error("Error: Not enough memory for permutation");
    }
    // artificially reduce available memory to avoid memory overflow
    // ideally, if whole PRSice is within the same memory pool, we will
    // not need to do this reduction
    const size_t available_memory = (valid_memory - used_memory) * 0.5;

    if (available_memory < basic_memory_required_per_thread) {
        fprintf(stderr, "\n");
        throw std::runtime_error("Error: Not enough memory for permutation");
    }
    if (available_memory / basic_memory_required_per_thread < num_thread) {
        num_thread = available_memory / basic_memory_required_per_thread;
    }

    Thread_Queue<std::vector<double>> set_perm_queue;

    // std::vector<double> null_p_value;
    int num_more_significant = 0;
    // if (store_null) null_p_value.reserve(num_perm);

    if (num_thread > 1) {
        std::thread producer(
            &PRSice::produce_null_prs, this, std::ref(set_perm_queue),
            std::ref(target), num_thread - 1, num_perm, set_size,
            num_selected_snps, obs_p_value, require_standardize);

        std::vector<std::thread> consumer_store;
        for (size_t i_thread = 0; i_thread < num_thread - 1; ++i_thread) {
            consumer_store.push_back(std::thread(
                &PRSice::consume_prs, this, std::ref(set_perm_queue),
                obs_p_value, std::ref(num_more_significant), is_binary,
                store_null));
        }
        producer.join();
        for (auto&& thread : consumer_store) thread.join();
    }
    else
    {
        null_set_no_thread(target, num_more_significant, num_perm, set_size,
                           num_selected_snps, obs_p_value, require_standardize,
                           is_binary, store_null);
    }
    double competitive_p =
        ((double) num_more_significant + 1.0) / ((double) num_perm + 1.0);
    m_prs_results[m_best_index].competitive_p = competitive_p;
}
*/
